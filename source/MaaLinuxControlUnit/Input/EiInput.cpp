#include "EiInput.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <linux/input-event-codes.h>
#include <optional>
#include <poll.h>
#include <string_view>
#include <thread>

#include <libei.h>

#include "MaaUtils/Logger.h"

MAA_CTRL_UNIT_NS_BEGIN

namespace
{

// fork: gamescope/libei 点击时序，硬编码为选定的修复行为（不依赖任何外部文件）：
//   press_split_ms   touch_down 中移动与按下拆分为两个 EI frame 的间隔（0 = 同 frame，stock 行为）
//   release_delay_ms touch_up 发送抬起 frame 前的额外等待，叠加在框架侧固定 50ms 之上
//   release_repeat   抬起 frame 的发送次数（幂等），用于对抗丢帧
// stock 行为为 0 / 0 / 1。一般无需设置；如需临时试验，可用环境变量
// MAA_EI_PRESS_SPLIT_MS / MAA_EI_RELEASE_DELAY_MS / MAA_EI_RELEASE_REPEAT 覆盖。
constexpr int kDefaultPressSplitMs = 30;
constexpr int kDefaultReleaseDelayMs = 100;
constexpr int kDefaultReleaseRepeat = 2;
constexpr int kMaxPressSplitMs = 1000;
constexpr int kMaxReleaseDelayMs = 5000;
constexpr int kMaxReleaseRepeat = 5;
constexpr int kReleaseRepeatGapMs = 5;

std::string_view trim(std::string_view text)
{
    constexpr std::string_view kSpaces = " \t\r\n";
    const auto begin = text.find_first_not_of(kSpaces);
    if (begin == std::string_view::npos) {
        return { };
    }
    const auto end = text.find_last_not_of(kSpaces);
    return text.substr(begin, end - begin + 1);
}

std::optional<long> parse_long(std::string_view text)
{
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }

    long value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc { } || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

int resolve_tuning(std::string_view key, const char* env_name, int default_value, int min_value, int max_value)
{
    long value = default_value;
    const char* source = "hardcoded";

    if (const char* raw = std::getenv(env_name); raw && raw[0] != '\0') {
        if (auto parsed = parse_long(raw)) {
            value = *parsed;
            source = "env";
        }
        else {
            LogWarn << "invalid env value, ignored" << VAR(env_name) << VAR(raw);
        }
    }

    const auto clamped = std::clamp(value, static_cast<long>(min_value), static_cast<long>(max_value));
    LogInfo << "EiInput tuning" << VAR(key) << VAR(clamped) << VAR(source);
    return static_cast<int>(clamped);
}

} // namespace

EiInput::EiInput(std::string eis_socket_path)
    : eis_socket_path_(std::move(eis_socket_path))
{
    LogFunc << VAR(eis_socket_path_);
}

EiInput::~EiInput()
{
    LogFunc;
    shutdown();
}

MaaControllerFeature EiInput::get_features() const
{
    return MaaControllerFeature_UseMouseDownAndUpInsteadOfClick | MaaControllerFeature_UseKeyboardDownAndUpInsteadOfClick;
}

bool EiInput::init()
{
    LogFunc;

    load_tuning();

    auto cleanup = [this]() {
        if (device_) {
            ei_device_unref(device_);
            device_ = nullptr;
        }
        if (ei_) {
            ei_unref(ei_);
            ei_ = nullptr;
        }
        ei_fd_ = -1;
        return false;
    };

    ei_ = ei_new_sender(nullptr);
    if (!ei_) {
        LogError << "ei_new_sender failed";
        return false;
    }

    ei_configure_name(ei_, "MaaFramework");

    int rc = ei_setup_backend_socket(ei_, eis_socket_path_.c_str());
    if (rc < 0) {
        LogError << "ei_setup_backend_socket failed" << VAR(rc) << VAR(eis_socket_path_);
        return cleanup();
    }

    ei_fd_ = ei_get_fd(ei_);
    if (ei_fd_ < 0) {
        LogError << "ei_get_fd failed";
        return cleanup();
    }

    constexpr int kPollMs = 100;
    constexpr int kMaxAttempts = 30;

    for (int i = 0; i < kMaxAttempts && !connected_; ++i) {
        int n = poll_and_dispatch(kPollMs);
        if (n < 0) {
            LogError << "poll failed during init";
            return cleanup();
        }
    }

    if (!connected_) {
        LogError << (device_ ? "EiInput init timed out" : "No device with keyboard and absolute pointer capabilities");
        return cleanup();
    }

    ei_device_start_emulating(device_, ++emul_seq_);
    ei_device_frame(device_, ei_now(ei_));

    LogInfo << "EiInput ready";
    return true;
}

void EiInput::shutdown()
{
    if (device_) {
        ei_device_stop_emulating(device_);
        ei_device_unref(device_);
        device_ = nullptr;
    }
    if (ei_) {
        ei_unref(ei_);
        ei_ = nullptr;
    }
    ei_fd_ = -1;
    connected_ = false;
}

void EiInput::load_tuning()
{
    press_split_ms_ = resolve_tuning("press_split_ms", "MAA_EI_PRESS_SPLIT_MS", kDefaultPressSplitMs, 0, kMaxPressSplitMs);
    release_delay_ms_ = resolve_tuning("release_delay_ms", "MAA_EI_RELEASE_DELAY_MS", kDefaultReleaseDelayMs, 0, kMaxReleaseDelayMs);
    release_repeat_ = resolve_tuning("release_repeat", "MAA_EI_RELEASE_REPEAT", kDefaultReleaseRepeat, 1, kMaxReleaseRepeat);
}

bool EiInput::click(int x, int y)
{
    LogError << "deprecated: get_features() returns MaaControllerFeature_UseMouseDownAndUpInsteadOfClick, "
                "use touch_down/touch_up instead"
             << VAR(x) << VAR(y);
    return false;
}

bool EiInput::swipe(int x1, int y1, int x2, int y2, int duration)
{
    LogError << "deprecated: get_features() returns MaaControllerFeature_UseMouseDownAndUpInsteadOfClick, "
                "use touch_down/touch_move/touch_up instead"
             << VAR(x1) << VAR(y1) << VAR(x2) << VAR(y2) << VAR(duration);
    return false;
}

bool EiInput::touch_down(int contact, int x, int y, int pressure)
{
    std::ignore = pressure;
    return pointer(EventPhase::Began, x, y, contact);
}

bool EiInput::touch_move(int contact, int x, int y, int pressure)
{
    std::ignore = pressure;
    return pointer(EventPhase::Moved, x, y, contact);
}

bool EiInput::touch_up(int contact)
{
    return pointer(EventPhase::Ended, 0, 0, contact);
}

bool EiInput::click_key(int key)
{
    LogError << "deprecated: get_features() returns MaaControllerFeature_UseKeyboardDownAndUpInsteadOfClick, "
                "use key_down/key_up instead"
             << VAR(key);
    return false;
}

bool EiInput::input_text(const std::string& text)
{
    return text_utf8(text);
}

bool EiInput::key_down(int key)
{
    return keyboard_key(EventPhase::Began, key);
}

bool EiInput::key_up(int key)
{
    return keyboard_key(EventPhase::Ended, key);
}

bool EiInput::scroll(int dx, int dy)
{
    // 对 dy 取反，在行为上和 Win32 保持一致
    return send({ EI_DEVICE_CAP_SCROLL }, [dx, dy](struct ei_device* d) {
        ei_device_scroll_delta(d, static_cast<double>(dx), static_cast<double>(-dy));
    });
}

bool EiInput::relative_move(int dx, int dy)
{
    return send({ EI_DEVICE_CAP_POINTER }, [dx, dy](struct ei_device* d) {
        ei_device_pointer_motion(d, static_cast<double>(dx), static_cast<double>(dy));
    });
}

int EiInput::poll_and_dispatch(int timeout_ms)
{
    pollfd pfd = { ei_fd_, POLLIN, 0 };
    int n = poll(&pfd, 1, timeout_ms);
    while (n < 0 && errno == EINTR) {
        n = poll(&pfd, 1, timeout_ms);
    }
    if (n > 0 && (pfd.revents & POLLIN)) {
        ei_dispatch(ei_);
        while (auto* event = ei_get_event(ei_)) {
            handle_event(event);
            ei_event_unref(event);
        }
    }
    return n;
}

void EiInput::handle_event(struct ei_event* event)
{
    switch (ei_event_get_type(event)) {
    case EI_EVENT_CONNECT:
        LogInfo << "EI_EVENT_CONNECT";
        break;
    case EI_EVENT_SEAT_ADDED: {
        struct ei_seat* seat = ei_event_get_seat(event);
        ei_seat_bind_capabilities(
            seat,
            EI_DEVICE_CAP_POINTER,
            EI_DEVICE_CAP_POINTER_ABSOLUTE,
            EI_DEVICE_CAP_KEYBOARD,
            EI_DEVICE_CAP_BUTTON,
            EI_DEVICE_CAP_SCROLL,
            EI_DEVICE_CAP_TEXT,
            nullptr);
        break;
    }
    case EI_EVENT_DEVICE_ADDED: {
        struct ei_device* dev = ei_event_get_device(event);
        if (ei_device_has_capability(dev, EI_DEVICE_CAP_KEYBOARD) && ei_device_has_capability(dev, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
            if (device_) {
                ei_device_unref(device_);
            }
            device_ = ei_device_ref(dev);
        }
        break;
    }
    case EI_EVENT_DEVICE_RESUMED:
        if (ei_event_get_device(event) == device_) {
            connected_ = true;
        }
        LogInfo << "EI_EVENT_DEVICE_RESUMED";
        break;
    case EI_EVENT_KEYBOARD_MODIFIERS:
        break;
    case EI_EVENT_DEVICE_PAUSED:
        LogInfo << "EI_EVENT_DEVICE_PAUSED";
        break;
    case EI_EVENT_DEVICE_REMOVED:
        if (ei_event_get_device(event) == device_) {
            ei_device_unref(device_);
            device_ = nullptr;
            connected_ = false;
        }
        LogInfo << "EI_EVENT_DEVICE_REMOVED";
        break;
    case EI_EVENT_SEAT_REMOVED:
        LogInfo << "EI_EVENT_SEAT_REMOVED";
        break;
    case EI_EVENT_DISCONNECT:
        LogWarn << "EI_EVENT_DISCONNECT";
        if (device_) {
            ei_device_unref(device_);
            device_ = nullptr;
        }
        connected_ = false;
        break;
    case EI_EVENT_SYNC:
        if (device_) {
            ei_device_frame(device_, ei_now(ei_));
        }
        break;
    default:
        LogTrace << "Unhandled ei event" << VAR(static_cast<int>(ei_event_get_type(event)));
        break;
    }
}

template <typename F>
bool EiInput::send(std::initializer_list<enum ei_device_capability> required_caps, F&& emit)
{
    if (!device_ || !ei_) {
        LogError << "send called without a valid device/ei";
        return false;
    }
    while (device_ && poll_and_dispatch(0) > 0) {
    }
    if (!device_) {
        LogError << "device removed or disconnected";
        return false;
    }
    for (enum ei_device_capability cap : required_caps) {
        if (!ei_device_has_capability(device_, cap)) {
            LogError << "device lacks required capability" << VAR(static_cast<int>(cap));
            return false;
        }
    }
    emit(device_);
    ei_device_frame(device_, ei_now(ei_));
    return true;
}

bool EiInput::pointer(EventPhase phase, int x, int y, int contact)
{
    int btn = BTN_LEFT;
    switch (contact) {
    case 1:
        btn = BTN_RIGHT;
        break;
    case 2:
        btn = BTN_MIDDLE;
        break;
    default:
        break;
    }

    switch (phase) {
    case EventPhase::Began:
        return pointer_began(x, y, btn);
    case EventPhase::Moved:
        return send({ EI_DEVICE_CAP_POINTER_ABSOLUTE }, [x, y](struct ei_device* d) {
            ei_device_pointer_motion_absolute(d, static_cast<double>(x), static_cast<double>(y));
        });
    case EventPhase::Ended:
        return pointer_ended(btn);
    default:
        break;
    }
    return false;
}

bool EiInput::pointer_began(int x, int y, int btn)
{
    if (press_split_ms_ <= 0) {
        LogTrace << "ei pointer began, single frame" << VAR(x) << VAR(y) << VAR(btn);
        return send({ EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_BUTTON }, [x, y, btn](struct ei_device* d) {
            ei_device_pointer_motion_absolute(d, static_cast<double>(x), static_cast<double>(y));
            ei_device_button_button(d, btn, true);
        });
    }

    // fork: 移动与按下拆成两个 frame 发送，避免合成器在同一帧内合并/丢失
    LogTrace << "ei pointer began, motion frame" << VAR(x) << VAR(y);
    if (!send({ EI_DEVICE_CAP_POINTER_ABSOLUTE }, [x, y](struct ei_device* d) {
            ei_device_pointer_motion_absolute(d, static_cast<double>(x), static_cast<double>(y));
        })) {
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(press_split_ms_));

    LogTrace << "ei pointer began, press frame" << VAR(btn) << VAR(press_split_ms_);
    return send({ EI_DEVICE_CAP_BUTTON }, [btn](struct ei_device* d) { ei_device_button_button(d, btn, true); });
}

bool EiInput::pointer_ended(int btn)
{
    // fork: 抬起前额外等待（叠加框架侧固定 50ms），并按配置重复发送抬起 frame（幂等，抗丢帧）
    if (release_delay_ms_ > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(release_delay_ms_));
    }

    bool ret = true;
    for (int i = 0; i < release_repeat_; ++i) {
        if (i > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kReleaseRepeatGapMs));
        }
        LogTrace << "ei pointer ended, release frame" << VAR(btn) << VAR(i + 1) << VAR(release_repeat_);
        ret = send({ EI_DEVICE_CAP_BUTTON }, [btn](struct ei_device* d) { ei_device_button_button(d, btn, false); }) && ret;
    }
    return ret;
}

bool EiInput::keyboard_key(EventPhase phase, int evdev_key)
{
    return send({ EI_DEVICE_CAP_KEYBOARD }, [phase, evdev_key](struct ei_device* d) {
        ei_device_keyboard_key(d, evdev_key, phase != EventPhase::Ended);
    });
}

bool EiInput::text_utf8(const std::string& text)
{
    return send({ EI_DEVICE_CAP_TEXT }, [&text](struct ei_device* d) { ei_device_text_utf8(d, text.c_str()); });
}

MAA_CTRL_UNIT_NS_END

# EiInput fork（gamescope/Libei 点击时序）

本仓库为 MaaFramework 的本地 fork，用于验证/规避 Linux-Gamescope（Libei/EIS）下
点击偶发丢失问题：按下（down）生效、抬起（up）丢失或被忽略，界面无变化，
单点击节点（`max_hit: 1`）在 reco 超时后失败。

- 基线：tag `v5.15.0-beta.1`（与 MaaEnd v2.34.0-beta.1 内置版本一致，版本字符串亦相同）
- 关联：MaaEnd/MaaEnd#6586（含完整日志与截图）

## 改动内容

仅动 `source/MaaLinuxControlUnit/Input/EiInput.{h,cpp}`（gamescope/libei 输入层）：

1. `touch_down`：移动与按下拆分为两个 EI frame 发送，间隔 `press_split_ms`；
2. `touch_up`：发送抬起 frame 前额外等待 `release_delay_ms`（叠加在框架侧固定 50ms 之上），
   并将抬起 frame 重复发送 `release_repeat` 次（幂等，用于对抗丢帧）；
3. 每个 frame 发送均有 `LogTrace`，可在 maafw.log 中核对时序；
4. 三个值**硬编码**在 `EiInput.cpp` 顶部（`kDefault*`），不依赖任何外部文件
   （MaaEnd 更新会清理 `maafw/` 内多余文件，配置文件不可靠）。

| 常量 | 值 | 上游 stock | 说明 |
| --- | --- | --- | --- |
| `kDefaultPressSplitMs` | 30 | 0 | 移动/按下分帧间隔（ms） |
| `kDefaultReleaseDelayMs` | 100 | 0 | 抬起前追加等待（ms） |
| `kDefaultReleaseRepeat` | 2 | 1 | 抬起 frame 重复次数（每次间隔 5ms） |

如需临时试验其他值（一般无需设置），可用环境变量覆盖：
`MAA_EI_PRESS_SPLIT_MS` / `MAA_EI_RELEASE_DELAY_MS` / `MAA_EI_RELEASE_REPEAT`。
恢复上游行为：三者改为 `0 / 0 / 1` 重新构建，或直接用官方 stock `.so` 回滚。

## 状态（2026-10-19）

- Maafend v2.34.0-rc.1 更新（05:15）整体刷新了 `maafw/`，删除了 `ei_fork.conf`，
  并可能覆盖 `libMaaLinuxControlUnit.so` —— 更新后需重新部署本 fork 的 `.so`。
- 实测结论：fork 生效时（日志确认 30/100/2 时序逐帧发送）**仍复现自然丢点击**：
  06:05:10.881 分帧点击 → 06:05:31.194 `GrowthChamberViewIn` 20s 识别超时 → 节点失败；
  失败前一步（ReceptionRoomExit ✕）与画面校验均正常。
  ⇒ 分帧 + 加长按住 + 双抬起 未能消除该问题，**时序不是根因**（或不是唯一根因）。
- **根因已定位（2026-10-19 受控复现）**：会客室关闭后存在约 **750ms 的"游戏侧点击忽略窗口"**——
  ✕ 后 600/700ms 点击培养舱必被忽略（复测确认），800ms 起必成功；与输入帧无关（同客户端同坐标窗口外全成功）。
  即：这是游戏在界面转场期间的输入忽略，不是丢帧/时序问题。修复应在 MaaEnd 管线侧
  （转场后 ≥800ms 再点，或单点击节点加重试），本 fork 的时序改动对本题无影响。
  详见 /tmp/opencode/ei-window-test.md 与 MaaEnd#6586。
- `libMaaLinuxControlUnit.so.orig` 为官方 stock 构建（同 tag、同工具链），用于对照/回滚。

## 构建

```sh
python3 tools/maadeps-download.py    # 首次：下载 MaaDeps（含 hermetic clang + sysroot）
cmake --preset "NinjaMulti Linux x64" -DMAA_HASH_VERSION=v5.15.0-beta.1
cmake --build build --config Release --target MaaLinuxControlUnit -j 24
```

产物：`build/bin/Release/libMaaLinuxControlUnit.so`。

注：`-DMAA_HASH_VERSION=v5.15.0-beta.1` 用于让 `MaaLinuxControlUnitGetVersion()`
与框架侧一致。PipeWire / libei 头文件与链接库取自宿主机，
其余依赖（OpenCV/onnxruntime/libc++ 等）来自 MaaDeps。

## 部署（换 .so）

```sh
cd /home/foxwhite/MaaEnd
install -m 644 MaaFramework/build/bin/Release/libMaaLinuxControlUnit.so maafw/.new.so \
  && mv -f maafw/.new.so maafw/libMaaLinuxControlUnit.so
# 重启 MaaEnd —— 进程启动时才 dlopen，运行中替换无效
```

验证 fork 生效（maafw.log）：

- `EiInput tuning [key=...] [value=...] [source=hardcoded]` 三行；
- `Click` 动作耗时由 ~51ms 变为 ~185-190ms（30 + 50 + 100 + 重复抬起间隔）；
- TRC 级别可见 `ei pointer began, motion/press frame`、`ei pointer ended, release frame` ×2。

## 回滚

```sh
cd /home/foxwhite/MaaEnd
install -m 644 maafw/libMaaLinuxControlUnit.so.orig maafw/.orig.so \
  && mv -f maafw/.orig.so maafw/libMaaLinuxControlUnit.so
```
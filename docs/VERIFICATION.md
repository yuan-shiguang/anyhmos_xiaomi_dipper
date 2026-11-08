# 验证范围 / Verification scope

下列记录来自本项目本地真机测试记录；原始设备日志、应用遥测、网络标识及屏幕内容未收入公开包。构建报告和文件哈希可以帮助识别产物，不能独立代替实物功能验收。

The following summarizes local device tests. Private raw logs, app telemetry, network identifiers and screenshots are excluded. Build reports and hashes identify artifacts; they do not independently establish hardware functionality.

> **适用范围提醒 / Scope note**：下列记录产生于本仓库改为米 8（dipper）之前的目标设备，其中涉及的 `super`、`vendor_boot`、`init_boot` 分区与对应启动路径在米 8 上**不存在**，不能作为本机验收依据。米 8 的静态分区、载荷位置与容量策略见[分区布局](DEVICE-PARTITIONS.md)。本机尚未刷入验证的功能一律按未完成处理。
>
> These records were produced on the target device used before this tree was retargeted to the Mi 8 (dipper). The `super`, `vendor_boot` and `init_boot` partitions they mention do not exist on dipper, so they are not acceptance evidence for this device. See the [partition layout](DEVICE-PARTITIONS.md); anything not yet flashed on the Mi 8 counts as unfinished.

## 普通 ext4 挂载修复 / Normal ext4 mount fix

移除数据分区挂载和持久日志中的固定 UUID、固定容量与布局标记限制。11 个隔离 loop 镜像用例通过，覆盖随机 UUID、无标记、原 UUID、已有文件保留、错误分区名、重复挂载及空白/非 ext4 分区；实际执行 ext4 挂载和原 C 存储身份检查。AArch64 bootstrap 编译通过，vendor_boot 解包往返及无关条目检查通过。此次未连接或刷写手机，不能将此前的桌面验收作为本次修复的真机结果。报告见 [userdata-ext4-fix-verification.json](../analysis/userdata-ext4-fix-verification.json)。

The fixed UUID, capacity and layout-marker prerequisites were removed from mounting and persistent logging. Eleven disposable loop-image cases passed, including real mounts and the original C storage identity check. The AArch64 bootstrap compiled and vendor_boot package checks passed. The phone was not accessed or flashed for this change; previous desktop acceptance does not validate the new image on hardware.

| 项目 / Item | 证据与结论 / Evidence and conclusion |
| --- | --- |
| 桌面 / Desktop | 多轮原生截图与用户操作确认；v145 自动启动约 172.10s 完成，原 BootControl 门槛后成功。Native desktop captures across rounds; v145 startup completed and passed its original boot-success gates. |
| 持久载荷 / Persistence | v118 主载荷迁 vendor_boot，最小 init_boot；v145 整分区读回 SHA 与构建值一致。Exact v145 partition readback matched its image hash. |
| 点击等待 / Tap stall | 同步 Binder 等待约 1922ms，局部 MSDP 修补后的对照约 7ms，用户确认改善。Not an end-to-end latency benchmark. |
| 电池 / Battery | v131 原 NDK capacity=94、plugged=2，与同轮 sysfs 及系统状态栏对应。No full charge/thermal/standby qualification. |
| Wi-Fi | v134 由 nl80211 读取真实协商能力并按代际上报，状态栏显示对应标识；没有更高代际的实链验证。Negotiated capability is not throughput. |
| 蓝牙 / Bluetooth | v139 设置开关 OFF/ON、原服务状态与发现附近设备；未配对未知外设，蓝牙音频未验。Discovery does not establish pairing or A2DP. |
| 音频 / Audio | v142 用户确认视频暂停/续播有声，真实 PAL 流关闭/重开有记录；v143 实际重启开机声音由用户确认。No exhaustive routing/quality test. |
| 媒体 / Media | v129 真实视频 surface 帧序列及 HEIC NDK 像素对照；属于软件路径/像素验证，不代表硬解或全部格式。 |
| EGL/WebView | v144 原生 fixture 复现首次无绘制 swap 的尺寸回退，修补后 fixture 消费正确像素；真实网页仍白屏。Fixture passing is not WebView acceptance. |
| 电源 / Power | 旧版已复现 OFF 后 late frame 重新 ACTIVE1；v145 捕获 `POWER_OFF_LATCH suppressed late frame without DRM commit`。用户最新实测明确确认熄屏已好，短按熄屏按此验收。Earlier explicit ON requests had possible manual interference; they do not override the latest physical acceptance. |
| USB | 多轮冷/普通重启完整回包；仍有角色切换/在途通信问题。60s 超限后的 `exit=-1 signal=9` 属于完整终止回复，已修主机误判。 |

## v145 验收边界 / v145 acceptance boundary

当前 v145 镜像为 100663296 字节，SHA-256 `bf39eaa2f45386d09621a9a4cfbbd4725e673cb046fb777d277a40eea70e6895`。相对 v143 只改变以下 ramdisk 条目：

- `dipper-compat-v118/graphics/native_display_wake`
- `dipper-compat-v118/graphics/libdisplay_composer_vdi_impl.z.so`
- `dipper-compat-v118/MANIFEST.json`

解包往返、无关条目、DTB、bootconfig 和 manifest 校验通过。实际刷入/读回并启动成功，**用户确认短按熄屏已正常**。长时间保持熄屏、多轮实体按键循环、深睡与异常恢复未完成完整验收，单独保留为待验证项。

Container roundtrip and unchanged-entry checks passed; the flashed partition matched and boot succeeded. **The user confirmed physical screen-off now works.** Prolonged standby, repeated physical-button cycles, deep sleep and abnormal recovery remain separate open acceptance items; this is not a claim of complete power-management parity.

## 历史与当前 / Historical versus current

模块分项可用不意味着每个新版都完成全部回归。蓝牙配对、蓝牙音频、硬解、WebView、TEE、相机和全面稳定性均不能标为完成。发生新的故障应保留旧验收的版本/场景范围，同时单独记录新问题，不能把历史成功外推成当前全通过。

Historical success is tied to its version and scenario. Use the main README's detailed matrix for open items. A diagnostic pattern, screenshot, service registration, return code or advertised capability must not be substituted for the corresponding user-visible acceptance test.

## 2026-10-31 源码整理检查 / Source cleanup checks

当前直接构建入口的 37 个目标已在准备好的 WSL 环境编译通过，UserIAM 4.0 后端、4.1 adapter 与驱动也已编译通过。Mesa/内核补丁可应用于记录的 upstream HEAD；串口 PTY 回归测试和 17 个 PowerShell 脚本的语法检查通过。构建产物不随源码提交。详见 `analysis/current-source-build-verification.json`。

All 37 direct build targets and the UserIAM backend/adapter/driver compiled in the prepared WSL environment. Selected Mesa/kernel patches passed application checks against the pinned upstream revisions; the serial PTY fixture and syntax checks for 17 PowerShell scripts passed. Full Mesa/kernel builds were not repeated. The subsequent rebuilt-runtime device test is described below.

## v146 重编译真机验收 / Rebuilt-runtime device acceptance

在本轮 37 个直接目标和 UserIAM 构建后，34 个运行文件装入 v146。设备三个启动分区先完整备份；仅在 TWRP 中写 vendor_boot_a，并核对整分区读回 SHA256。重启约 171.73 秒完成，音频、Wi-Fi、蓝牙启动 exit=0；桌面解锁通过并生成原生系统截图。用户确认既有应正常的功能均正常并授权发布。未完成项目仍按原边界标注，没有声称新增硬解、WebView 或安全能力。

After compilation, 34 runtime files were packaged as v146. The device boot trio was backed up; only vendor_boot_a was written, and its complete readback matched. Reboot startup completed in about 171.73 seconds; audio/Wi-Fi/Bluetooth startup exited successfully, desktop unlock passed and a native system screenshot was captured. The user accepted the previously expected working features and authorized publication. Previously unfinished features remain open. See `analysis/rebuilt-source-v146-verification.json`.

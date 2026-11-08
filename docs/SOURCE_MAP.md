# 当前功能源码 / Current component sources

每个功能只保留当前已验证基线中最完善的实现。选择依据是实际刷入的 v146 载荷和分项验收，不是版本号最大。旧试验、旧实现、逐版构建链已从当前仓库树删除。完整逐文件来源见 `SOURCE-SELECTION.json`。

数据分区挂载与持久日志检查已在该基线上修正，支持正常格式化的 ext4；此修复已通过离线测试，尚待真机刷入。参见 [USERDATA.md](USERDATA.md)。

One selected implementation is retained per component, together with the shared code, generated interfaces and upstream patches it needs. Incomplete features remain incomplete; the rebuilt-runtime v146 acceptance is documented in docs/VERIFICATION.md.

| 功能 / Component | 当前源码 / Current sources | 选取依据 / Selection |
| --- | --- | --- |
| 内核接口 / Kernel | `compat/kernel/`, `compat/ipc/` | 4.9 官方内核上的类型化 IPC 导出；5.15 GKI overlay 仅作历史保留。 |
| Binder / AccessToken | `compat/ipc/` | 实际使用的 v16 类型化导出实现；删除旧 kprobe 解析实现。 |
| 显示、亮度、熄屏 / Display, brightness, screen power | `compat/display/composer_vdi.cpp`, `native_display_wake.cpp` | v145 compositor，已含亮度/帧节奏和 OFF latch；唤醒 helper v2。 |
| Buffer / 媒体元数据 | `compat/display/buffer_vdi.cpp` | v130，包含此前 crop/YUV/RGB 与全局 alpha 支持。 |
| GPU / EGL | `external/patches/source--mesa3d.patch`, `external/overlays/source--mesa3d/` | Turnip v128，已部署 EGL v126；删除 v144 EGL 实验和额外 GPU 调频。 |
| 触控 / Touch | `compat/touch/`（仅文档） | 米 8 触控由官方内核内置 ST FTS（`fts@49`）/ mXT640U 驱动以标准 MT-B 上报，不再有桥接程序；器件依据见 `compat/touch/README.md`。 |
| 音频 / Audio | `compat/audio/` | v142 VDI、PAL server/client/lifecycle、服务发布、PD mapper，以及 sdm845 用的 tinyalsa 后端（`tinyalsa_server.c`，静态链接，直接对接内核 PCM）。各司其职，非重复音频实现。 |
| Wi-Fi | `compat/wifi/`, `runtime/compat/graphics/start-wifi-v116.sh` | sdm845 用内核内置 qcacld-3.0 + icnss；qcacld-2.0 属 msm8974/8994，不适用。 |
| 蓝牙 / Bluetooth | `compat/bluetooth/`, `runtime/compat/bluetooth-v136/` | v139 当前检查接口、仍需要的 v136 后端，以及 v141 能力纠正。接口文件名延续原运行路径。 |
| 电池 / Battery | `compat/battery/` | v131 真电量 HDI 发布与对照读取。 |
| 账户与数据 / Account and database | `compat/framework/account/`, `database/` | account HMAC 和 storage launcher、database v2；无旧 database v1。 |
| UserIAM | `compat/useriam/` | 当前软件 4.0 backend + 4.1 ABI adapter；两层都被当前代码调用，不能把 4.0 误删为旧试验。 |
| 服务等待与内存 / Service waits and memory | `compat/framework/memmgr/`, `runtime/compat/graphics/apply-memmgr-v2.sh`, `apply-msdp-local-v1.sh` | 最新实际加载的修补脚本与汇编；不再需要旧版脚本生成新版。 |
| 启动、按键与桌面 / Startup and desktop | `compat/bootstrap/`, `compat/services/`, `runtime/` | v118 最小入口、v121 USB probe、v143 启动声音逻辑及当前锁屏 helper v5。 |
| BootControl | `compat/bootcontrol/`, `runtime/compat/graphics/mark-boot-success-v135.sh` | 原 HAL 与桌面验收门槛。 |
| 分区布局与打包 / Partition layout and packaging | `tools/dipper_partitions.json`, `tools/build_dipper_images.py`, `docs/DEVICE-PARTITIONS.md` | 官方整包 `partition.xml` 导出的单槽静态布局；打包只处理 `boot`/`system`/`vendor`/`cust`，镜像超限直接报错而不截断。 |
| 调试 / Host tools | `output/*.ps1`, `tests/` | 当前串口/传输/截图、四个静态分区的备份恢复及当前有意义的协议/分配器测试。 |

`runtime/` 中脚本和配置从 v146 ramdisk 的最终内容提取，文件名保留被 init 和启动程序引用的名称。文件名中的 v116/v136 等不表示仓库同时维护多套旧实现。原厂库、固件、应用、编译产物和完整镜像不在仓库内。

Runtime names are preserved for compatibility with the accepted image. Interface generations, shared helpers and staged patches needed by the current system are dependencies, not alternate historical implementations. Removed experiments remain recoverable from Git history; the current tree contains the selected sources only.

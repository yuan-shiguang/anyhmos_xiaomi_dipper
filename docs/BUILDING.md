# 当前源码构建 / Building the selected sources

源码已按功能收敛，构建直接使用最终文件，不再从 v1、v2 逐轮生成后续源码。当前仍是研发源码，并非完整 ROM 或从零一键刷机工具。

The tree keeps the selected implementation of each component. It does not include proprietary inputs, full upstream repositories or flashable images. v146 device acceptance is documented separately; this is not a claim of byte-for-byte reproducibility for every component.

## 外部输入 / External inputs

- dipper 官方固件的匹配内核配置、模块和固件，以及项目使用的 MatePad Air HarmonyOS donor 链接库；自行准备，不随仓库分发。
- Harmony native SDK `6.0.0.47`、AArch64 Linux GCC/strip，以及需要的 Android Bionic/C++ 链接环境。
- `external/upstream-sources.json` 记录的上游源码版本；原有许可位于 `external/licenses/`。
- libdrm 静态库、匹配的生成接口头文件、PAL/OpenSSL 等组件输入。部分接口头已收录。

米 8 没有 `super`、`vendor_boot`、`init_boot`，也没有 `vendor_dlkm` 分区：内核模块在 `vendor/lib/modules`。`tools/build_dipper_images.py` 因此不再调 lpmake，改为静态分区打包：donor 的 `system`/`vendor` 按 `tools/dipper_partitions.json` 的容量校验后原样拷贝，载荷树用 `mke2fs -d` 打成 `cust` 的 ext4 镜像，`boot.img` 只做 `ANDROID!` v1 / page 4096 与 64 MiB 上限校验。任何镜像超限都会直接报错并打印超出的字节数，不会截断；`system` 扩容后需要显式传 `--system-bytes`（4 KiB 对齐且不小于分区表值）。`tools/Flash-Dipper.ps1` 在刷写前比对同一份容量，单槽设备没有 `current-slot`，脚本按无后缀分区名刷写并跳过 `set_active`。

```sh
# 1) 生成兼容层载荷树（同时仍产出 vendor_boot 镜像，供对照/回滚）
python3 tools/build_dipper_ramdisk.py ... --payload-out work/payload

# 2) 把 system/vendor/cust/boot 打成可刷的四件套
python3 tools/build_dipper_images.py \
  --donor <华为整包解包根> --boot <4.9 内核编出的 boot.img> \
  --payload work/payload \
  --output output/images --work work/images
```

`--payload` 指向的目录需要包含 `compat/`（由上面第一步导出，必须有 `compat/bin/init`），
以及可选的 `hmos/sys_prod`、`hmos/preload`（由 donor 的 `sys_prod.img`、`preload.img`
解包成目录）。缺 `hmos/` 时构建仍会通过，开机日志会打印 staged 分区缺席，系统按没有
`sys_prod`/`preload` 的方式启动。

Boot image updates require the matching development baseline. Recovery follows the established procedure of restoring this device's own backed-up `boot`/`system`/`vendor`/`cust` images first.

全新安装的数据分区要求及最新挂载修复见 [USERDATA.md](USERDATA.md)。普通 ext4 格式化即可准备文件系统；不需要专用 UUID 或标记，启动程序不会自动格式化。The updated mount logic accepts normally formatted ext4 and never formats automatically; the corresponding image still needs device testing.

## 构建入口 / Build entry points

在 WSL 使用 `tools/build_current.py`。它只编译显式选定的目标，不自动刷写或连接设备。

```sh
python3 tools/build_current.py --list
python3 tools/build_current.py composer allocator display-wake --dry-run
python3 tools/build_current.py composer allocator display-wake
python3 tools/build_current.py audio-vdi audio-loader audio-manager
python3 tools/build_current.py audio-tinyalsa
python3 tools/build_current.py bootstrap init-entry wifi-category
```

默认编译输出为 `work/current-build/`，构建命令和产物哈希写入其 `build-report.json`。`--dry-run` 可先检查依赖路径。当前脚本覆盖常用适配目标；未提供目标的源文件仍需按原接口与工具链自行构建，不可据此声称完整 ROM 已能一键重建。

| 环境变量 / Variable | 默认位置 / Default |
| --- | --- |
| `DIPPER_DEPS` | `/root/hmos-work/graphics-deps` |
| `DIPPER_SDK` | `/root/hmos-work/sdk-6.0.0.47/installed/native` |
| `DIPPER_UPSTREAM` | `$DIPPER_DEPS/source` |
| `DIPPER_DONOR_LINK` | 仓库 `work/donor-hdi-link` |
| `DIPPER_STAGE` | `$DIPPER_DEPS/stage` |
| `DIPPER_BIONIC_LINK` | 仓库 `work/audio-bionic-v1/lib64` |
| `DIPPER_PAL_INCLUDE` | `/root/hmos-reference/pal-sm8550-a13/inc` |
| `DIPPER_ANDROID_CPP_HEADERS` | `/root/hmos-work/clang-prebuilt/clang-r450784e/include/c++/v1` |
| `DIPPER_BT_VENDOR_LINK` | 仓库 `work/donor-bluetooth-v136/vendor/lib64` |

音频有两个后端，按平台二选一。`pal-server`/`pal-lifecycle` 需要 `DIPPER_PAL_INCLUDE` 指向 sm8550 的 PAL 头，只能在有 `/dev/agm` 的平台上用；米 8 的 sdm845 没有 AGM，改用 `audio-tinyalsa` 编出的 `dipper-tinyalsa-server`，它静态链接、只依赖内核 UAPI（`compat/audio/tinyalsa_server.c` 里自带所需结构体并用 `_Static_assert` 对齐 4.9 内核的尺寸与 ioctl 号），不需要任何厂商库或头文件。产物放在 `--audio-build-dir` 里打包时会随 `dipper-audio-v1/audio/native/bin/` 一起进载荷（米 8 上落在 cust 分区的 `compat/` 下）；缺这个产物时载荷保持原样，开机日志会明确报 `AUDIO_BACKEND_ABSENT_NO_TINYALSA`。真机调试可用 `dipper-tinyalsa-server --dump-mixer` 列出全部混音器控件，再用 `--route <文件>` 覆盖内置路由表（每行 `控件名 = 值 [值...]`）。

`compat/ipc/Makefile` 对应当前模块，应在匹配内核源码和配置下构建。本机运行 4.9 官方内核，`external/patches/hmos-work--gki-dipper-exports.patch` 与对应 overlay 属于 5.15 GKI 时期的历史记录，当前不参与构建；改用 4.9 树重新导出即可。保留 CFI、符号版本与产品/输入哈希校验。

Mesa 补丁以记录的 upstream HEAD 为基础，EGL 的 `platform_ohos.c` 使用已部署 v126 源码；Turnip `tu_image.cc` 与 v128 源码快照已比对一致。应用 overlay 后，按 Mesa 自身的构建流程配置 Turnip/Zink。原 v144 EGL 实验不再保留。

UserIAM 使用 `tools/build_useriam.py`，需自行准备 target-compatible OpenSSL 头、4.0/4.1 donor stub 和 HDF host 库。`DIPPER_USERIAM_VENDOR_LIBS`、`DIPPER_USERIAM_HOST_LIBS`、`DIPPER_OPENSSL_INCLUDE` 可指定路径。`compat/useriam/software-backend.patch` 描述当前软件后端所需的上游差异，需审查应用到匹配来源；其 4.0 后端和 4.1 adapter 共同组成当前实现，不应互相删减。

## 配置与补丁 / Runtime and patches

`runtime/` 保留当前 v146 脚本与配置，`patches/donor/` 是由最终已部署库和原始 donor 输入生成的合并修补配方。它们保留全文件输入/输出 SHA-256 和逐段 before/after 校验，可用 `tools/apply_guarded_elf_patch.py` 生成副本。原库不分发。

Some runtime scripts intentionally apply dependent stages during startup. Those stages are part of the current boot path, not independently selectable old versions. Runtime checksum constants identify the accepted binaries; newly rebuilt outputs may differ and require independent packaging checks and device validation. Do not blindly copy build output into a ramdisk or remove guards to force a mismatch through.

## 调试与检查 / Debugging and checks

COM probe 不是 ADB。用 `output/Invoke-ProbeCommand.ps1`、文件传输和截图脚本操作；串口请求必须串行。单请求上限约 60 秒，超时完整回复不等于失联。已有测试机数据不应按旧流程清除。

`tools/verify_sources.py` 校验当前源码清单、语法、链接和本地依赖；`tools/refresh_manifest.py` 只更新哈希清单，不打包、不上传。功能验收仍以 [VERIFICATION.md](VERIFICATION.md) 和双语 README 为准。

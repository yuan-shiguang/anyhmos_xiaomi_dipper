# 米 8（dipper）实际分区布局

本文件记录的是**官方 MIUI V12.5.2.0.QEACNXM（Android 10）整包**里的真实分区表，
来源是 `images/partition.xml`（Qualcomm `rawprogram*.xml` 配套的 GPT 描述）。
机器可读版本在 [`tools/dipper_partitions.json`](../tools/dipper_partitions.json)，
构建与刷写工具都以它为准，不要凭记忆写分区名或容量。

## 单槽静态分区

Android 侧**全部单槽**：`boot`、`system`、`vendor`、`cust`、`modem`、`dsp`、`dtbo`、`vbmeta`
都没有 `_a`/`_b` 后缀，也没有 `current-slot`。只有引导链是 A/B 的：

```
abl aop cmnlib cmnlib64 devcfg hyp keymaster qupfw storsec tz xbl xbl_config
```

所以刷机脚本**不能**给 Android 分区拼槽位后缀，也不能调用 `set_active`。

## 这个设备没有的分区

```
super  vendor_boot  init_boot  system_ext  product  odm  vendor_dlkm  metadata  vbmeta_system
```

这一点很关键，仓库里凡是假设这些分区存在的代码都必须改：

- 没有 `super` → 不存在 LP metadata / dm-linear 逻辑分区，`system` 和 `vendor` 是**静态 ext4 分区**，直接按 `by-name` 挂载。
- 没有 `vendor_boot`、`init_boot` → 没有独立的兼容层 ramdisk 分区，`boot` 只剩约 21 MiB 余量，装不下 Mesa/Turnip 这类大件，载荷改放 `cust`。
- 没有 `metadata` → 不能 `fastboot erase metadata`（米 8 上这条命令会失败），持久日志改用 `cust`。
- 没有 `vendor_dlkm` → 内核模块都在 `vendor/lib/modules`。

## 关键分区

| 分区 | 容量 | 原厂镜像 | 格式 | 本仓库用途 |
| --- | --- | --- | --- | --- |
| `system` | 3072 MiB | `system.img` 3.00 GiB | sparse ext4 | HarmonyOS 系统 |
| `vendor` | 1024 MiB | `vendor.img` 1.00 GiB | sparse ext4 | HarmonyOS vendor（含 `lib/modules/qca_cld3_wlan.ko`、`firmware/wlan/qca_cld`、`etc/acdbdata`） |
| `cust` | 832 MiB | `cust.img` 0.81 GiB | sparse ext4 | **兼容层载荷**（Mesa/Turnip、音频库、脚本） |
| `boot` | 64 MiB | `boot.img` 45.5 MB | `ANDROID!` v1，page 4096 | 内核 + 兼容层 ramdisk |
| `recovery` | 64 MiB | `recovery.img` 59 MB | `ANDROID!` v1 | 原厂 recovery，保留不动 |
| `userdata` | 出厂 4096 MiB | `userdata.img` | sparse ext4 | 整分区直接挂 `/data`，不格式化 |
| `bluetooth` | 1 MiB | `bluetooth.img` | **FAT16** | 蓝牙固件，独立分区 |
| `persist` | 64 MiB | `persist.img` | ext4 raw | 传感器校准等，保留不动 |
| `modem` / `dsp` | 192 / 32 MiB | `modem.img` / `dsp.img` | raw | 基带与 ADSP，保留不动 |

`boot.img` 现有 ramdisk 只有 796,946 字节（约 778 KiB），内核 44.7 MB。
`boot` 分区 64 MiB，去掉原 ramdisk 后兼容层 ramdisk 的预算约 **21 MiB** ——
这就是载荷不放 `boot` 而放 `cust` 的原因。

## 载荷位置的决定

`cust`（832 MiB，原厂只占 151 MB）承载兼容层载荷，`boot` 的 ramdisk 只留
`hmos-probe` 与少量启动脚本。启动时 `hmos-probe` 直接读 `/cust/compat`，
不再需要 `vendor_boot`。这样：

- `boot` 分区不会被撑爆，Mesa/Turnip 这类大件有充足空间；
- HarmonyOS 不使用 `cust`，不存在争用；
- 代价是刷机多写一个分区，`Flash-Dipper.ps1` 已经把它列进清单。

### cust 内的目录结构

```
/cust/
├── compat/                  # 兼容层载荷，启动时绑到 initramfs 的 /compat
│   ├── bin/                 # dipper-tinyalsa-server 等
│   ├── lib64/               # 显示/音频 VDI
│   ├── graphics/            # 启动脚本与调优配置
│   ├── etc/                 # 配置与清单
│   ├── busybox
│   └── hmos-init-debug.cfg
├── hmos/
│   ├── sys_prod/            # donor 的 sys_prod 解包成目录
│   └── preload/             # donor 的 preload 解包成目录
└── hmos-debug/              # 持久日志（没有 metadata 分区时的落点）
```

`sys_prod` 和 `preload` 在米 8 上没有对应分区，所以以**目录**形式放在 cust 上，
启动时 bind 到 `/hmos/{sys_prod,preload}` 并 remount 成只读。`system`、`vendor`
是真实分区，`by-name` 节点由内核分区表直接提供，不经过 device-mapper。

`compat/` 这棵树由 `tools/build_dipper_ramdisk.py --payload-out` 从最终的
vendor_boot cpio 导出，映射规则是：`dipper-compat-v118/graphics/x → compat/graphics/x`，
原 ramdisk 的 `compat/x → compat/x`，`busybox`/`hmos-init-debug.cfg`/`graphics/*`
则落到 `compat/` 下的同名位置；`init`、`lib/modules` 等仍留在 `boot` 的 ramdisk 里，
不重复导出。导出时会检查 `compat/bin/init` 是否存在，缺了直接报错。

### 启动时的挂载顺序

1. `discover()` 从 `/sys/class/block/*/uevent` 建立 `/dev/block/by-name/*`；
2. `static_partitions_at("/dev/block/by-name","/hmos")` 把 `system`、`vendor`
   按 ext4 → erofs → f2fs 顺序试挂成**只读**，两个都挂上才算成功；
3. `cust_payload_at()` 把 `cust` 挂成**可写**（载荷脚本要 chmod 自己的文件），
   然后把 `/cust/compat` 整体 bind 到 `/compat`（第二阶段用 `/compat/bin/init` 启动），
   再把 `graphics`、`busybox`、`hmos-init-debug.cfg` 分别 bind 到根上；
   `/bin`、`/lib64`、`/etc` 属于 initramfs 自己，**不**做整目录展开绑定；
   载荷里没有 `bin/init` 时直接判失败，不进入半启动状态；
4. `sys_prod`、`preload` bind 到 `/hmos/` 并转只读；
5. `hmos_root()` 用 overlay 组新根，`/compat` 以 RAM overlay 挂进 `/hmos-root/compat`，
   脚本对载荷的改写只落在内存里，不会写坏 cust 上的原件。

## 容量策略

`tools/build_dipper_images.py` 对每个镜像都做**展开后大小 ≤ 分区容量**的校验，
超限直接报错并打印超出的字节数，绝不截断：

- `system`：需要扩容时先改分区表并刷入，再用 `--system-bytes <容量>` 告诉构建器
  新容量（必须是 4 KiB 倍数且不小于分区表值）。分区表没扩容而镜像超限时构建会停。
- `vendor`、`cust`：只能缩小载荷，没有扩容余量。
- `boot`：固定 64 MiB，只做 `ANDROID!` v1 / page 4096 与容量校验，不重打包内核。

## 复核方法

真机上可以直接读设备自己的分区表，和本文件对照：

```sh
adb shell 'cat /proc/partitions'
adb shell 'ls -l /dev/block/by-name/'
adb shell 'cat /proc/asound/cards'
```

Fastboot 侧核对容量（`partition-size:` 是十六进制字节数）：

```sh
fastboot getvar partition-size:system
fastboot getvar partition-size:vendor
fastboot getvar partition-size:cust
fastboot getvar partition-size:boot
```

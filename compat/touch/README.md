# 米 8（dipper）触控适配资料

本目录不再包含触控算法或桥接程序。米 8 的触控由官方内核内置驱动直接以标准 Linux 多点触控（MT-B）上报，
HarmonyOS 侧只需要读 evdev 节点，因此原先为 `sheng`（小米平板 6S Pro 12.4，Novatek NT36532 + `/dev/xiaomi-touch`
原始帧 mmap）编写的算法桥在 dipper 上没有可用输入源，已整体移除。

## 内核实测依据

来源：`kernel_xiaomi_sdm845`（小米 8 / sdm845 官方内核源码，本地只读查阅）。

| 项目 | 内容 |
| --- | --- |
| 内核配置 | `arch/arm64/configs/vendor/xiaomi/dipper.config`：`CONFIG_TOUCHSCREEN_ST_FTS_V521=y`、`CONFIG_TOUCHSCREEN_ATMEL_MXT=y` |
| 编译方式 | 两个驱动均为 `=y`（编进内核镜像），`mi845_defconfig` 中没有任何 `=m` 触控模块；不需要 `insmod` |
| 主面板节点 | `arch/arm64/boot/dts/qcom/dipper-p0.dtsi` 的 `fts@49`：`compatible = "st,fts"`，I2C 地址 `0x49`（`&qupv3_se14_i2c`），IRQ GPIO 31、RST GPIO 32，固件名 `st_fts_e1.ftb` |
| 备选面板节点 | 同一总线的 `atmel_mxt_ts_640u@4a`：`compatible = "atmel,mxt-ts"`，mXT640U，I2C 地址 `0x4a` |
| 驱动文件 | `drivers/input/touchscreen/fts/fts.c`、`drivers/input/touchscreen/atmel_mxt_ts.c` |
| 设备名 | `fts.c` 中 `info->input_dev->name = FTS_TS_DRV_NAME`，即 `/sys/class/input/event*/device/name` 为 `fts` |
| 上报协议 | `input_mt_init_slots(info->input_dev, TOUCH_ID_MAX, INPUT_MT_DIRECT)` + `input_report_key(BTN_TOUCH)`，标准 MT-B 槽位协议 |
| 坐标范围 | `drivers/input/touchscreen/fts/fts.h`：`X_AXIS_MAX 1080`、`Y_AXIS_MAX 2160`；`ABS_MT_TOUCH_MAJOR/MINOR` 与 `ABS_MT_PRESSURE` 上限 2048 |
| 固件更新 | 由用户态 daemon 触发（`fts_flash_procedure`），正常开机探测不依赖文件系统里的 `.ftb`，无需把固件放进 `/compat` |

两块面板共用同一组 GPIO 与中断，两个节点在设备树里同时使能，实际只有焊装的那一颗能在 I2C 上应答，
另一颗探测失败后自动退出，因此运行时只会出现一个触控输入设备。

## 运行时的实际动作

`compat/bootstrap/probe_graphics_v118.inc` 已经把 `/sys/class/input/event*` 全部映射到
`/hmos-root/dev/input/`，并设置 `chown 6696:6696`、`chmod 0660`（多模输入服务读取输入节点所用的 uid/gid），
这一步对内置 FTS 驱动注册的节点同样生效。

`runtime/compat/graphics/apply-touch.sh` 现在只做两件事：确认 `fts`/`atmel` 节点存在；若当前内核是模块化的
（节点不存在），依次从 `/compat/modules`、`vendor_dlkm/lib/modules`、`vendor/lib/modules` 里补 `insmod fts.ko`
或 `atmel_mxt_ts.ko`，然后把节点映射进 `/hmos-root/dev/input`。原先解包 `touch-bundle.tar.gz`、加载
`xiaomi_touch.ko`/`nt36532_touch.ko`、校验 Novatek 固件的那一套只对 `sheng` 成立，已随 Novatek 桥一起删除。

## 真机复核方法

```sh
# 确认设备名与坐标范围（应为 fts，X 0..1080，Y 0..2160）
cat /sys/class/input/event*/device/name
getevent -p /dev/input/event2
# 确认节点已发布进 HarmonyOS 根
ls -l /hmos-root/dev/input/
```

如果真机上 `fts` 节点不存在，先查内核日志里的 `fts` 探测失败原因（供电 `disp_vddio_vreg`/`pm8998_l28`、
GPIO 31/32 复用），而不是回退到 Novatek 方案。

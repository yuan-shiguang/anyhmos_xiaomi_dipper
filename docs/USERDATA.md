# 用户数据分区 / Userdata

全新安装前，在 Recovery 中把 **userdata 格式化为 ext4**，然后使用包含新挂载逻辑的启动镜像。只擦除分区会留下空白块设备，需要实际创建文件系统。格式化会清除原数据。

For a fresh installation, **format userdata as ext4 in Recovery**, then use a boot image containing the updated mount logic. An erase alone leaves a blank block device; a filesystem must be created. Formatting removes existing data.

启动程序通过分区名确认 userdata，读取实际容量和设备号，拒绝重复挂载，并确认最终挂载的是该分区上的 ext4。UUID 由正常格式化流程生成，不再要求固定值，也不再读取 `.dipper-userdata-layout` 作为开机条件。已有标记可以保留，不影响挂载。

The bootstrap checks the partition name, actual size and device number, rejects an already-mounted partition and verifies the resulting ext4 mount. Any normally generated UUID is accepted. The old `.dipper-userdata-layout` file is no longer a boot prerequisite and can remain on existing installations.

首次开机会创建系统需要的数据目录，保留开机向导。后续开机直接使用已有文件，挂载程序不运行格式化、不删除数据，也不重写文件系统 UUID。持久日志只在确认真实 userdata 已挂载后写入，避免写到早期的临时内存目录。

First boot creates required data directories and retains the setup wizard. Later boots use existing files. The mount code does not format, delete data or change the filesystem UUID. Persistent logging checks the real userdata mount instead of relying on a marker or writing to the early RAM placeholder.

## 检查与构建 / Testing and building

在 Linux/WSL 中以 root 运行以下测试。测试会创建临时文件和 loop 设备，在独立挂载命名空间检查新 UUID、无标记、已有文件保留、错误分区名、重复挂载和非 ext4 文件系统。sysfs 分区名使用测试数据，文件系统和挂载操作是真实执行，不连接手机。

Run these tests as root in Linux/WSL. They use disposable loop images in a private mount namespace. The partition-name metadata is a fixture; filesystem detection, mounts, UUIDs and data-preservation checks use real operations. The phone is not accessed.

```sh
python3 tests/test_userdata_mount.py
python3 tools/build_current.py bootstrap
```

更新镜像时需要同时放入新 bootstrap 程序和 `runtime/compat/graphics/init-userdata-ext4.sh`，并更新载荷校验清单。米 8 上 bootstrap 在 `boot` 分区，脚本在 `cust` 分区的 `compat/` 下，因此这一项修复只需要重打 `boot.img` 和 `cust.img`，`system`、`vendor` 不受影响。旧镜像仍会检查固定 UUID，单独更新 GitHub 源码不会改变设备上的程序。

Repack `boot.img` with the rebuilt bootstrap and `cust.img` with the updated mount script, then refresh the payload manifest. On the Mi 8 the bootstrap lives on `boot` and the scripts under `compat/` on `cust`, so this fix needs no change to `system` or `vendor`. Existing images retain the old UUID restriction until replaced; updating the repository alone does not update a device.

当前完成的是编译、隔离镜像测试和镜像打包检查，尚未在手机上刷入本次修复。详见[检查报告](../analysis/userdata-ext4-fix-verification.json)。

Compilation, isolated image tests and package checks are complete. This fix has not yet been flashed to the phone. See the [verification report](../analysis/userdata-ext4-fix-verification.json).

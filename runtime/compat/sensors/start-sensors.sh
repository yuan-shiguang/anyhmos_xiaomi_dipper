#!/busybox sh
# ADSP就绪后在独立根目录启动原传感器进程
set -eu
B=/busybox
PACKAGE=${SENSOR_STARTUP_PACKAGE:-/dipper-sensors}
STATE=${SENSOR_DATA_DIR:-/hmos-root/data/service/el1/public/dipper_sensors}
L=/hmos-root/system/lib/ld-musl-aarch64.so.1

if [ "${1-}" != private ]; then
    exec "$B" unshare -m "$B" sh "$0" private
fi
original_umask=$(umask)
umask 077
"$B" mount --make-rprivate /
# 首次userdata尚未由nativeinit建目录时沿用其原模式
for parent in /hmos-root/data/service /hmos-root/data/service/el1 /hmos-root/data/service/el1/public; do
    if [ ! -d "$parent" ]; then
        "$B" mkdir -m 0711 "$parent"
        "$B" chown 0:0 "$parent"
    fi
done
"$B" mkdir -p "$STATE"
"$B" chown 0:1000 "$STATE"
"$B" chmod 0750 "$STATE"
exec 9> "$STATE/launcher.lock"
"$B" flock -n 9 || exit 75
exec > "$STATE/startup.log" 2>&1
stage=prepare
source_mounted=0
source_mount=

finish()
{
    result=$?
    trap - EXIT INT TERM HUP
    set +e
    if [ "$source_mounted" = 1 ]; then
        "$B" umount "$source_mount" || result=74
    fi
    echo "SENSOR_STARTUP_EXIT result=$result stage=$stage"
    exit "$result"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

# 首次从真实只读persist播种，已有缓存不再覆盖
if [ ! -d "$STATE/sensors" ]; then
    stage=persist-seed
    seed=$("$B" mktemp -d "$STATE/.seed-XXXXXX")
    source_mount=$seed/source-persist
    "$B" mkdir -p "$source_mount" "$seed/root/mnt/vendor/persist/sensors"
    "$B" mount -t ext4 -o ro,noload /dev/block/by-name/persist "$source_mount"
    source_mounted=1
    "$B" cp -a "$source_mount/sensors/." "$seed/root/mnt/vendor/persist/sensors/"
    "$B" umount "$source_mount"
    source_mounted=0
    SENSOR_COLD_BASE=$seed "$B" sh "$PACKAGE/prepare-private-persist.sh"
    "$B" mv "$seed/root/mnt/vendor/persist/sensors" "$STATE/sensors"
    "$B" rmdir "$source_mount" "$seed/root/mnt/vendor/persist" "$seed/root/mnt/vendor" \
        "$seed/root/mnt" "$seed/root" "$seed"
    echo SENSOR_CACHE_SEEDED
else
    echo SENSOR_CACHE_REUSED
fi

# 运行时资产位于私有tmpfs，命名空间退出后自动释放
stage=runtime-root
D=$STATE/runtime
R=$D/root
"$B" mkdir -p "$D"
"$B" mount -t tmpfs -o mode=0700,nosuid,nodev tmpfs "$D"
# 原库目录沿启动时掩码，私有日志的077不传给原进程
umask "$original_umask"
"$B" mkdir -m 0755 "$R"
"$B" tar -xzf "$PACKAGE/root.tar.gz" -C "$R"
"$B" mount --bind "$STATE/sensors" "$R/mnt/vendor/persist/sensors"

# 沿原冷启动路径装载通信模块，节点编号只从真实sysfs读取
stage=fastrpc
if [ ! -d /sys/module/frpc_adsprpc ]; then
    "$B" insmod /lib/modules/frpc-adsprpc.ko
fi
"$B" sh "$PACKAGE/stage-fastrpc-nodes.sh"

stage=mounts
for name in sys proc; do
    "$B" mount --bind "/$name" "$R/$name"
    "$B" mount -o remount,bind,ro "$R/$name"
done
"$B" mount --bind /hmos-root/dev "$R/dev"
for name in dsp vendor/dsp vendor/lib/rfsa/adsp; do
    "$B" mount --bind /compat/original-firmware/image "$R/$name"
    "$B" mount -o remount,bind,ro "$R/$name"
done

# exec保留监督器PID；FD9的自有锁贯穿整个运行周期
stage=supervise
LD_PRELOAD= LD_LIBRARY_PATH=/hmos-root/system/lib64 \
    exec "$L" "$PACKAGE/native_sensorspd_launcher" "$R"

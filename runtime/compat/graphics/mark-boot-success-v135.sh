#!/busybox sh
# The original device HAL is invoked only after this boot's successful desktop.
set -eu
B=/busybox
R=/hmos-root
D=/compat/bootcontrol-v135
[ "$#" -eq 1 ] || exit 31
[ "$1" = "$($B cat /proc/sys/kernel/random/boot_id)" ] || exit 32
# 内核为 4.9 官方内核：不再按 5.15 GKI 版本号门禁，改由设备标识与模块存在性判断
$B grep -q '^androidboot.hardware.sku = "dipper"$' /proc/bootconfig
completed=$($B chroot "$R" /system/bin/param get bootevent.boot.completed)
[ "$(echo "$completed" | $B tr -d ' \r\n')" = true ] || exit 34
# 仅接受状态接口确认解锁后产生的完整记录
$B grep -Eq '^SCREENLOCK_VERIFIED after=0 (callback=-?[0-9]+( elapsed=[0-9]+)?|already_unlocked=1)$' \
 "$R/data/local/tmp/v116-autostart.txt" || exit 36
cd "$D"
$B sha256sum -c SHA256SUMS
if [ -e /dev/block/bootdevice ] || [ -L /dev/block/bootdevice ]; then
 [ "$($B readlink -f /dev/block/bootdevice)" = /dev/block ] || exit 35
else
 $B ln -s /dev/block /dev/block/bootdevice
fi
$B timeout 8 $B env LD_LIBRARY_PATH="$D:/compat/audio-runtime/lib64" \
 LD_PRELOAD="$D/libdipper_bootcontrol_guarded_v135.so" DIPPER_MARK_SUCCESS_BOOT_ID="$1" \
 /compat/audio-runtime/bin/linker64 /compat/audio-runtime/bin/agmplay -h

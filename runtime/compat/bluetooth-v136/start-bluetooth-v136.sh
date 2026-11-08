#!/busybox sh
# Original303 native backend and original OHOS HDI/service; no vendor writes.
set -eu
B=/busybox
R=/hmos-root
S=/compat/bluetooth-v136
D=$R/data/local/tmp
stage=gate
trap 'code=$?; echo "BT_START_EXIT=$code STAGE=$stage"' EXIT
# 内核为 4.9 官方内核：不再按 5.15 GKI 版本号门禁，改由设备标识与模块存在性判断
boot_id=$($B cat /proc/sys/kernel/random/boot_id)
echo "$boot_id"
# Audio uses several of the same stock modules. Wait for its loader to finish.
stage=wait_audio
i=0
while ! $B grep -q '^AUDIO_START_EXIT=' "$D/v116-audio-startup.log"; do
 [ "$i" -lt 25 ] || exit 21
 i=$((i+1)); $B sleep 1
done
stage=integrity
(cd "$S" && $B sha256sum -c SHA256SUMS)
stage=modules
# 米 8 的 BT 走 hci_qca + msm_geni_serial（官方内核编入），没有 btpower 设备；
# 模块化内核时才从暂存目录补加载，缺失不算失败。
for ko in btqca.ko hci_uart.ko msm_geni_serial.ko; do
 for d in $R/compat/modules $R/vendor_dlkm/lib/modules $R/vendor/lib/modules; do
  if [ -f "$d/$ko" ]; then echo "BT_MODULE_LOAD=$d/$ko"; $B insmod "$d/$ko" || echo "BT_MODULE_FAILED=$ko"; fi
 done
done
stage=nodes
for spec in tty/ttyHS0 mem/urandom; do
 n=${spec##*/}
 v=$($B cat /sys/class/$spec/dev)
 ma=${v%:*}; mi=${v#*:}
 [ -n "$ma" ] && [ -n "$mi" ]
 mode=0600; [ "$n" != urandom ] || mode=0666
 for root in /dev "$R/dev"; do
  if [ ! -e "$root/$n" ]; then $B mknod -m "$mode" "$root/$n" c "$ma" "$mi"; fi
 done
done
stage=readonly_inputs
# 米 8 是单槽静态分区：没有 slot_suffix，按分区名直接取，不绑定具体 block 号
btpart=$($B readlink -f /dev/block/by-name/bluetooth)
persist=$($B readlink -f /dev/block/by-name/persist)
[ -n "$btpart" ] && [ -b "$btpart" ] || exit 23
[ -n "$persist" ] && [ -b "$persist" ] || exit 24
$B mkdir -p /compat/original-bluetooth /compat/original-persist /mnt/vendor
if ! $B grep -q ' /compat/original-bluetooth ' /proc/mounts; then
 $B mount -t vfat -o ro "$btpart" /compat/original-bluetooth
fi
if ! $B grep -q ' /compat/original-persist ' /proc/mounts; then
 $B mount -t ext4 -o ro,noload "$persist" /compat/original-persist
fi
$B grep -q "^$btpart /compat/original-bluetooth vfat ro," /proc/mounts
$B grep -q "^$persist /compat/original-persist ext4 ro," /proc/mounts
echo 'e757354efe4af4260b47ca98da9023e8d90120f3652e836dfb2b72c5d9fb2720  /compat/original-bluetooth/image/hmtbtfw20.tlv' | $B sha256sum -c -
echo '8c54ce84da45b71d8d5ec4063cd3d508a4083e0dd0dfb9e11fb7baed383a51e4  /compat/original-bluetooth/image/hmtnv20.bin' | $B sha256sum -c -
if [ ! -e /bt_firmware ]; then $B ln -s /compat/original-bluetooth /bt_firmware; fi
if [ ! -e /mnt/vendor/persist ]; then $B ln -s /compat/original-persist /mnt/vendor/persist; fi
[ "$($B readlink -f /bt_firmware)" = /compat/original-bluetooth ] || exit 25
[ "$($B readlink -f /mnt/vendor/persist)" = /compat/original-persist ] || exit 26
[ "$($B stat -c %s /compat/original-persist/bluetooth/.bt_nv.bin)" -eq 6 ] || exit 27
# OEM file stores the display order. ReadBDAddr independently returned its reverse.
$B mkdir -p "$R/data/vendor/bluetooth" "$R/data/log/bt" "$R/data/service/el1/public/bluetooth"
$B chown 3021:3021 "$R/data/vendor/bluetooth"
$B chmod 0770 "$R/data/vendor/bluetooth"
$B chown 1002:1002 "$R/data/log/bt" "$R/data/service/el1/public/bluetooth"
$B chmod 0711 "$R/data/service/el1/public/bluetooth"
set -- $($B od -An -tu1 /compat/original-persist/bluetooth/.bt_nv.bin)
[ "$#" -eq 6 ] || exit 28
printf '%02X:%02X:%02X:%02X:%02X:%02X' "$1" "$2" "$3" "$4" "$5" "$6" > "$R/data/vendor/bluetooth/btmac.txt.new"
$B chown 3021:3021 "$R/data/vendor/bluetooth/btmac.txt.new"
$B chmod 0640 "$R/data/vendor/bluetooth/btmac.txt.new"
$B mv "$R/data/vendor/bluetooth/btmac.txt.new" "$R/data/vendor/bluetooth/btmac.txt"
stage=backend
[ ! -S "$R/dev/unix/socket/dipper.bt.h4" ] || exit 29
$B setsid $B env LD_LIBRARY_PATH="$S/lib64:/compat/audio-runtime/lib64" LD_PRELOAD="$S/libdipper_bt_bridge_v136.so" DIPPER_BT_ORIGINAL_LIBRARY="$S/lib64/android.hardware.bluetooth@1.0-impl-qti.so" /compat/audio-runtime/bin/linker64 /compat/audio-runtime/bin/agmplay -h > "$D/dipper-bluetooth-backend-v136.txt" 2>&1 </dev/null &
p=$!
echo "$p" > "$D/dipper-bluetooth-backend-v136.pid"
i=0
while ! $B grep -q '^BT_BRIDGE_LISTENING=1 ' "$D/dipper-bluetooth-backend-v136.txt"; do
 [ -d /proc/$p ] && [ "$i" -lt 30 ] || exit 30
 i=$((i+1)); $B sleep 0.1
done
stage=port_ready_contract
# The donor sets this after its connectivity file/permission setup, not RF init.
# Apply the equivalent real Qualcomm port setup before notifying the old service.
[ "$($B stat -c '%a:%u:%g' "$R/data/vendor/bluetooth")" = 770:3021:3021 ]
[ "$($B stat -c '%a:%u:%g' "$R/data/service/el1/public/bluetooth")" = 711:1002:1002 ]
[ "$($B stat -c '%a:%u:%g' "$R/dev/unix/socket/dipper.bt.h4")" = 660:0:3021 ]
[ -c /dev/ttyHS0 ] && [ -S "$R/dev/unix/socket/dipper.bt.h4" ]
$B mkdir -p "$R/data/service/el1/public/fcm"
fcm_id=$($B awk -F: '$1=="fusion_connectivity_manager_ext" {print $3}' "$R/system/etc/passwd")
[ -n "$fcm_id" ]
$B chown "$fcm_id:$fcm_id" "$R/data/service/el1/public/fcm"
$B chmod 0700 "$R/data/service/el1/public/fcm"
$B chroot "$R" /system/bin/param set vendor.setup_mpxx_dir_mode success
echo BT_REAL_PORT_DIRECTORY_READY=1
stage=native_services
$B chroot "$R" /system/bin/param set dipper.debug.start.bluetooth true
stage=complete
echo BT_NATIVE_SERVICE_REQUESTED=1

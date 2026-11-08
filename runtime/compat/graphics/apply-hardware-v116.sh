#!/bin/sh
set -eu
B=/busybox
R=/hmos-root
# DIPPER_DISPLAY_LOG_RETENTION_BEGIN
PRESERVE_DISPLAY_LOGS=0
hardware_log_boot_id=unknown
hardware_log_uptime=unknown
if [ "$PRESERVE_DISPLAY_LOGS" = 1 ]; then
 hardware_log_boot_id=$($B cat /proc/sys/kernel/random/boot_id 2>/dev/null) || hardware_log_boot_id=unknown
 hardware_log_uptime=$($B cut -d ' ' -f 1 /proc/uptime 2>/dev/null) || hardware_log_uptime=unknown
fi
# 追加启动边界，日志失败不阻断硬件准备
stage_hardware_log() {
 if [ "$PRESERVE_DISPLAY_LOGS" = 1 ]; then
  if [ -L "$1" ] || { [ -e "$1" ] && [ ! -f "$1" ]; }; then
   echo "DIPPER_DISPLAY_LOG_INVALID_PATH=$1" >&2 || :
   return 0
  fi
  if ! printf '\nDIPPER_DISPLAY_LOG_STAGE boot_id=%s uptime=%s pid=%s\n' \
   "${hardware_log_boot_id:-unknown}" "${hardware_log_uptime:-unknown}" "$$" >> "$1"; then
   echo "DIPPER_DISPLAY_LOG_APPEND_FAILED=$1" >&2 || :
  fi
  if ! $B chown "$2" "$1"; then
   echo "DIPPER_DISPLAY_LOG_CHOWN_FAILED=$1" >&2 || :
  fi
  if ! $B chmod "$3" "$1"; then
   echo "DIPPER_DISPLAY_LOG_CHMOD_FAILED=$1" >&2 || :
  fi
 else
  : > "$1"
  $B chown "$2" "$1"
  $B chmod "$3" "$1"
 fi
}
# DIPPER_DISPLAY_LOG_RETENTION_END
$B mkdir -p $R/compat/etc $R/compat/bin
for name in init-hardware-v116.cfg powermgr.json; do
 $B cp /graphics/$name $R/compat/etc/$name
 $B chmod 0644 $R/compat/etc/$name
done
for name in dipper_hardware_display hardware_observe native_brightness_launcher native_sa_check native_battery_hdi_publisher_v131; do
 $B cp /graphics/$name $R/compat/bin/$name
 $B chmod 0755 $R/compat/bin/$name
done
# Only the actual composer host group receives hardware backlight write access.
$B chown 0:3036 /sys/class/backlight/panel0-backlight/brightness
$B chmod 0660 /sys/class/backlight/panel0-backlight/brightness
$B mkdir -p $R/data/service/el0/stats $R/data/service/el0/thermal/config
$B chown -R 5528:5528 $R/data/service/el0/stats $R/data/service/el0/thermal
echo DIPPER_V102_DISPLAY_AND_HARDWARE_SERVICES_STAGED_VENDOR_UNTOUCHED

stage_hardware_log "$R/data/local/tmp/dipper-brightness-stderr.txt" 5528:5528 0600

stage_hardware_log "$R/data/local/tmp/dipper-composer-metrics.txt" 3036:3036 0640

$B touch $R/data/local/tmp/dipper-gpu-policy-disabled

stage_hardware_log "$R/data/local/tmp/dipper-battery-hdi.txt" 3025:3025 0600

# Only wifi_host receives this adapter; original vendor libraries remain intact.
$B mkdir -p $R/compat/lib64
$B cp /graphics/libdipper_wifi_category_v134.so $R/compat/lib64/libdipper_wifi_category_v134.so
$B chmod 0644 $R/compat/lib64/libdipper_wifi_category_v134.so
stage_hardware_log "$R/data/local/tmp/dipper-wifi-category-v134.txt" 3026:3026 0600

# Keep Boot Control isolated from the original vendor and the HMOS linker.
$B mkdir -p /compat/bootcontrol-v135
$B cp /dipper-compat-v118/bootcontrol-v135/* /compat/bootcontrol-v135/
$B chmod 0644 /compat/bootcontrol-v135/*

# Bluetooth remains isolated from both the vendor partition and its linker.
$B mkdir -p /compat/bluetooth-v136 $R/compat/bluetooth-v136/ohos
$B cp -R /dipper-compat-v118/bluetooth-v136/. /compat/bluetooth-v136/
# /compat is already the RAM root compat alias; do not copy onto itself.
$B cp /compat/bluetooth-v136/native_bt_hdi_publisher_v136 $R/compat/bin/
$B cp /compat/bluetooth-v136/native_bt_host_check_v138 $R/compat/bin/
$B cp /compat/bluetooth-v136/native_bt_host_check_v139 $R/compat/bin/
$B chmod 0755 $R/compat/bin/native_bt_host_check_v139
$B chmod 0755 $R/compat/bin/native_bt_host_check_v138
$B chmod 0755 $R/compat/bin/native_bt_hdi_publisher_v136
for file in init-bluetooth-v136.cfg bluetooth_service_v136.json; do
 $B cp /compat/bluetooth-v136/$file $R/compat/etc/$file
done
stage_hardware_log "$R/data/local/tmp/dipper-bluetooth-hdi.txt" 3021:3021 0600

stage_hardware_log "$R/data/local/tmp/dipper-bluetooth-check.txt" 2000:2000 0600

# Original SoftBus profile in the RAM system overlay.
$B cp /compat/bluetooth-v136/softbus_server.json $R/system/profile/softbus_server.json

# dipper has no NearLink hardware. Correct only the donor capability in RAM.
capability=$R/sys_prod/etc/param/hw_defaults.para
capability_sha=$($B sha256sum "$capability")
case "$capability_sha" in
 47e94ce212bf301531e1e0f6480f9b4c566296a557e8fc03b44d958ed81fe8a2*)
  $B cp /graphics/hw_defaults-v141.para "$capability"
  $B chmod 0644 "$capability"
  ;;
 3fa79d36281510333d96de8ff23844e40229b1d227ada18ca0eff2fbf24a9dc2*) ;;
 *) echo DIPPER_V141_CAPABILITY_HASH_MISMATCH >&2; exit 1 ;;
esac
echo DIPPER_V141_NEARLINK_UNSUPPORTED_RAM_CAPABILITY

# 暂存用户域默认值，交给原设置服务初始化
$B cp /graphics/dipper/default_settings-pcmode.json "$R/sys_prod/etc/settings_data/default_settings.json"
$B chmod 0644 "$R/sys_prod/etc/settings_data/default_settings.json"

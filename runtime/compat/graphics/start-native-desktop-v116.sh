#!/busybox sh
# The validated v78 startup order, executed on-device without a host command.
set -eu
B=/busybox
R=/hmos-root
stage=prepare
now() { read seconds rest < /proc/uptime; echo "${seconds%.*}"; }
trace_stage() {
 read stage_seconds stage_idle < /proc/uptime
 echo "AUTOSTART_STAGE name=$stage uptime=$stage_seconds"
}
autostart_exit() {
 code=$?
 $B chroot "$R" /system/bin/param set dipper.debug.start.bootanim false >/dev/null 2>&1 || :
 echo "AUTOSTART_EXIT code=$code stage=$stage"
 if [ "$code" -eq 0 ] && [ "$stage" = complete ]; then
  if $B sh /graphics/mark-boot-success-v135.sh "$boot_id" > "$R/data/local/tmp/dipper-boot-success-v135.txt" 2>&1; then
   echo AUTOSTART_ORIGINAL_BOOTCONTROL_SUCCESS=1
  else
   mark_code=$?
   echo "AUTOSTART_BOOTCONTROL_NOT_MARKED code=$mark_code"
  fi
  $B setsid $B timeout 75 $B sh /compat/bluetooth-v136/start-bluetooth-v136.sh > "$R/data/local/tmp/dipper-bluetooth-start-v136.txt" 2>&1 </dev/null &
  $B sleep 0.2
 fi
 exit "$code"
}
trap autostart_exit EXIT
trace_stage
echo AUTOSTART_BEGIN_VERSION=v116
echo AUTOSTART_SOURCE_REBUILD=v146
echo AUTOSTART_REVISION=v132_ORIGINAL_BOOT_VIDEO_AND_TIMINGS
boot_id=$($B cat /proc/sys/kernel/random/boot_id)
$B cat /proc/sys/kernel/random/boot_id /proc/uptime
$B grep -q '^dipper_hmos_ipc ' /proc/modules
stage=hilog
trace_stage
i=0
while ! $B test -S "$R/dev/unix/socket/hilogInput"; do
 i=$((i+1)); [ "$i" -lt 40 ] || exit 21; $B sleep 1
done
stage=core
trace_stage
for gate in coredeps storaged; do $B chroot "$R" /system/bin/param set dipper.debug.start.$gate true; done
$B sleep 2
stage=memmgr
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.memmgr true
deadline=$(($(now)+12))
while ! $B timeout 3 $B chroot "$R" /compat/bin/native_sa_check 1909; do
 if [ "$(now)" -ge "$deadline" ]; then echo MEMMGR_NOT_READY=1; break; fi
 $B sleep 1
done
stage=core
trace_stage
result=$($B timeout 15 $B chroot "$R" /system/bin/sdc filecrypt init_main_user)
echo "$result"
echo "$result" | $B grep -q 'ret: 0'
for gate in storage kvdata useriam account appspawn theme wallpaper ui; do
 $B chroot "$R" /system/bin/param set dipper.debug.start.$gate true
done
$B setsid $B timeout 900 $B chroot "$R" /system/bin/hilog -L I,W,E,F > "$R/data/local/tmp/v116-live.txt" 2>&1 </dev/null &
stage=diagnostics
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.diagnostics true
stage=deviceproviders
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.deviceproviders true
stage=database
trace_stage
deadline=$(($(now)+90))
i=0
while ! $B timeout 3 $B chroot "$R" /data/local/tmp/native_sa_dump 1301 "/data/local/tmp/v116-database-$boot_id-$i.txt" -l; do
 i=$((i+1)); [ "$(now)" -lt "$deadline" ] || exit 22; $B sleep 1
done
echo AUTOSTART_DATABASE_READY
stage=time
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.time true
deadline=$(($(now)+15))
while ! $B timeout 3 $B chroot "$R" /compat/bin/native_sa_check 3702; do
 if [ "$(now)" -ge "$deadline" ]; then echo TIME_SERVICE_NOT_READY=1; break; fi
 $B sleep 1
done
stage=framework
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.framework true
for id in 1602 404; do
 deadline=$(($(now)+8))
 while ! $B timeout 3 $B chroot "$R" /compat/bin/native_sa_check "$id"; do
  if [ "$(now)" -ge "$deadline" ]; then echo "FRAMEWORK_PROVIDER_NOT_READY=$id"; break; fi
  $B sleep 1
 done
done
stage=media
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.media true
deadline=$(($(now)+40))
while ! $B timeout 3 $B chroot "$R" /compat/bin/native_sa_check 3002; do
 [ "$(now)" -lt "$deadline" ] || exit 35
 $B sleep 1
done
echo AUTOSTART_ORIGINAL_MEDIA_SA3002_READY
stage=codec
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.codec true
deadline=$(($(now)+20))
while ! $B timeout 3 $B chroot "$R" /compat/bin/native_sa_check 3011; do
 if [ "$(now)" -ge "$deadline" ]; then echo AVCODEC_NOT_READY=1; break; fi
 $B sleep 1
done
# Initialize the real WLAN hosts before UI code can request WiFi SAs.
stage=wireless
trace_stage
if $B timeout 50 $B sh "$R/compat/bin/start-wifi-v116.sh" > "$R/data/local/tmp/v116-wifi-startup.log" 2>&1; then
 echo AUTOSTART_WLAN_REQUESTED=1
else
 echo AUTOSTART_WLAN_REQUEST_FAILED=1
fi
stage=audio
trace_stage
if [ -f /dipper-audio-v1/SHA256SUMS ]; then
 $B setsid $B timeout 120 $B sh /graphics/start-audio-v116.sh > "$R/data/local/tmp/v116-audio-startup.log" 2>&1 </dev/null &
 echo AUTOSTART_AUDIO_REQUESTED=1
fi
stage=netstack
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.netstack true
for id in 1120 1124 1126 1158 1151; do
 deadline=$(($(now)+15))
 while ! $B timeout 3 $B chroot "$R" /compat/bin/native_sa_check "$id"; do
  if [ "$(now)" -ge "$deadline" ]; then echo "PLATFORM_SA_NOT_READY=$id"; break; fi
  $B sleep 1
 done
done
stage=graphics
trace_stage
$B sh /graphics/start-graphics-v116.sh
stage=hardware
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.hardware true
stage=bootanimation
trace_stage
# Donor IsNormalBoot explicitly supports UNKNOWN. A missing imported reason
# otherwise makes its original player force volume zero. Preserve every real
# reason and the original persistent sound preference / volume policy.
boot_reason=$($B chroot "$R" /system/bin/param get ohos.boot.reboot_reason 2>/dev/null || :)
case "$boot_reason" in
 ""|"Get parameter "*)
  $B chroot "$R" /system/bin/param set ohos.boot.reboot_reason UNKNOWN
  echo AUTOSTART_BOOT_REASON_MISSING_FALLBACK=UNKNOWN
  ;;
esac
deadline=$(($(now)+8))
while :; do
 if $B timeout 2 $B chroot "$R" /compat/bin/native_sa_check 10 &&
    $B timeout 2 $B chroot "$R" /compat/bin/native_sa_check 3001 &&
    $B timeout 2 $B chroot "$R" /compat/bin/native_sa_check 3011; then
  $B chroot "$R" /system/bin/param set bootevent.boot.completed false
  $B chroot "$R" /system/bin/param set dipper.debug.start.bootanim true
  echo AUTOSTART_ORIGINAL_BOOT_VIDEO_REQUESTED=1
  break
 fi
 if [ "$(now)" -ge "$deadline" ]; then echo AUTOSTART_BOOT_VIDEO_SKIPPED_DEPENDENCY=1; break; fi
 $B sleep 1
done
stage=touch
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.touch true
# 触控节点在 probe_graphics 阶段由 apply-touch.sh 发布（米 8 用内核内置 FTS/mXT640U 驱动，
# 没有桥进程可等），这里只确认节点数量，不阻塞启动。
touch_nodes=$($B ls "$R/dev/input" 2>/dev/null | $B wc -l) || touch_nodes=0
echo "TOUCH_INPUT_NODE_COUNT=$touch_nodes"
$B chroot "$R" /system/bin/param set dipper.debug.start.input true
stage=uideps
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.uideps true
for id in 3701 2902; do
 deadline=$(($(now)+20))
 while ! $B timeout 3 $B chroot "$R" /compat/bin/native_sa_check "$id"; do
  if [ "$(now)" -ge "$deadline" ]; then echo "UI_DEPENDENCY_NOT_READY=$id"; break; fi
  $B sleep 1
 done
done
stage=scene
trace_stage
$B cp "$R/data/local/tmp/foundation-app-core.json" "$R/system/profile/foundation.json"
$B chroot "$R" /system/bin/param set dipper.debug.start.screen true
$B chroot "$R" /system/bin/param set dipper.debug.start.compconfig true
stage=inputmethod
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.start.ime true
deadline=$(($(now)+25))
while ! $B timeout 3 $B chroot "$R" /compat/bin/native_sa_check 3703; do
 if [ "$(now)" -ge "$deadline" ]; then echo INPUTMETHOD_NOT_READY=1; break; fi
 $B sleep 1
done
stage=scene
trace_stage
deadline=$(($(now)+600))
i=0
while :; do
 path="/data/local/tmp/v116-scene-$boot_id-$i.txt"
 if $B timeout 3 $B chroot "$R" /data/local/tmp/native_sa_dump 4606 "$path" -a &&
    $B grep -qE 'SCBScreenLock|SCBDesktop' "$R$path"; then break; fi
 i=$((i+1)); [ "$(now)" -lt "$deadline" ] || exit 32; $B sleep 1
done
stage=wake
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.display.wake true
$B sleep 2
stage=unlock
trace_stage
$B chroot "$R" /system/bin/param set dipper.debug.screen.unlock true
i=0
while ! $B grep -q 'SCREENLOCK_VERIFIED after=0' "$R/data/local/tmp/dipper-capture/unlock-request.txt"; do
 i=$((i+1)); [ "$i" -lt 40 ] || exit 33; $B sleep 1
done
$B cat "$R/data/local/tmp/dipper-capture/unlock-request.txt"
stage=bootcomplete
trace_stage
# Account, SceneBoard and original screen-unlock verification have completed.
# Let the original player finish and release its layer before capturing desktop.
$B chroot "$R" /system/bin/param set bootevent.boot.completed true
echo AUTOSTART_REAL_BOOT_COMPLETED_EVENT=1
stage=capture
trace_stage
# Allow asynchronous icon decoding; this is a native system capture, not a draw test.
$B sleep 20
if [ -n "$($B pidof bootanimation)" ]; then
 echo AUTOSTART_BOOT_VIDEO_EXIT_FALLBACK=1
 $B chroot "$R" /system/bin/param set dipper.debug.start.bootanim false
 $B sleep 1
fi
$B chroot "$R" /system/bin/param set dipper.debug.start.capture true
i=0
while :; do
 state=$($B chroot "$R" /system/bin/param get startup.service.ctl.dipper_snapshot)
 if [ "$state" -eq 5 ] 2>/dev/null && $B test -s "$R/data/local/tmp/dipper-capture/display0.jpeg"; then break; fi
 i=$((i+1)); [ "$i" -lt 20 ] || exit 34; $B sleep 1
done
$B cp "$R/data/local/tmp/dipper-capture/display0.jpeg" "$R/data/local/tmp/v116-autostart-desktop.jpeg"
stage=complete
trace_stage
echo AUTOSTART_SERVICES_READY=1
echo 'DESKTOP_CONTENT_AND_PHYSICAL_DISPLAY_REQUIRE_EXTERNAL_VERIFICATION=1'
$B cat /proc/uptime

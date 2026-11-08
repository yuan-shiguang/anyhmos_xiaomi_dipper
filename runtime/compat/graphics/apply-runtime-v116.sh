set -e
B=/busybox
R=/hmos-root
$B cp /graphics/native_screenlock_request $R/compat/bin/native_screenlock_request
echo "bffd5494d4e8040c48fb8a1c496d7b9b73d0006de72935fa9a894d5670f93b8b  $R/compat/bin/native_screenlock_request" | $B sha256sum -c -
$B chmod 0755 $R/compat/bin/native_screenlock_request
$B mkdir -p $R/data/local/tmp/dipper-capture
$B rm -f $R/data/local/tmp/dipper-capture/screenlock-cycle-test $R/data/local/tmp/dipper-capture/screenlock-callback-test
$B rm -f $R/data/local/tmp/dipper-capture/display0.jpeg $R/data/local/tmp/v116-autostart-desktop.jpeg
: > $R/data/local/tmp/dipper-capture/unlock-request.txt
$B chown 2000:2000 $R/data/local/tmp/dipper-capture/unlock-request.txt
$B touch $R/data/local/tmp/dipper-gpu-owner-cleanup
$B chmod 0644 $R/data/local/tmp/dipper-gpu-owner-cleanup
$B mkdir -p $R/compat/oobe-offline-v1
$B cp $R/sys_prod/etc/oobe/hwupgrade_extend_pages.json $R/compat/oobe-offline-v1/hwupgrade_extend_pages.json
$B cp /graphics/oobe_page_offline.json $R/compat/oobe-offline-v1/oobe_page.json
$B chmod 0755 $R/compat/oobe-offline-v1
$B chmod 0644 $R/compat/oobe-offline-v1/*.json
$B mount --bind $R/compat/oobe-offline-v1 $R/sys_prod/etc/oobe
# 米 8 触控由内核内置 FTS/mXT640U 驱动上报 evdev，bootstrap 已把 /dev/input/event* 发布到
# $R/dev/input（6696:6696、0660），这里不再加载 xiaomi_touch.ko 或改写 xm_htc_report_coordinate。
echo DIPPER_V82_RUNTIME_STAGED_VENDOR_UNTOUCHED

$B sh /graphics/apply-hardware-v116.sh

$B sh /graphics/apply-time-v1.sh

$B sh /graphics/apply-memmgr-v2.sh

$B sh /graphics/apply-cli-wait-v1.sh

$B sh /graphics/apply-db-key-v2.sh

$B sh /graphics/apply-audio-wait-v1.sh

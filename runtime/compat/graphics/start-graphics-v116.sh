set -e
B=/busybox
R=/hmos-root
# 本机没有独立 vendor_dlkm 分区时，模块位于 vendor/lib/modules
M=$R/vendor_dlkm/lib/modules; [ -d "$M" ] || M=$R/vendor/lib/modules
for n in gpucc-kalama msm_performance qcom_va_minidump coresight msm_kgsl; do
 mod=$(echo "$n" | $B tr - _)
 if $B grep -q "^$mod " /proc/modules; then continue; fi
 # 米 8 的 KGSL/性能相关驱动编进官方内核，没有对应 .ko；缺失即视为内置，不中断启动
 if [ ! -f "$M/$n.ko" ]; then echo "DIPPER_KERNEL_BUILTIN=$n"; continue; fi
 $B insmod "$M/$n.ko"
done
nums=$($B cat /sys/class/kgsl/kgsl-3d0/dev)
if [ ! -c $R/dev/kgsl-3d0 ]; then $B mknod $R/dev/kgsl-3d0 c "${nums%:*}" "${nums#*:}"; fi
$B chown 0:1003 $R/dev/kgsl-3d0
# Native applications have per-app groups; KGSL enforces per-process contexts.
$B chmod 0666 $R/dev/kgsl-3d0
$B chroot $R /system/bin/param get dipper.debug.start.coredeps
$B sleep 2
$B chroot $R /system/bin/param set dipper.debug.start.render true
$B chroot $R /system/bin/param set dipper.debug.start.installs true
$B sleep 2
$B ps | $B grep -E 'host|render_service|accesstoken|installs'
$B ls -ld $R/data/service/el1/public/access_token
$B chroot $R /system/bin/hilog -x -T SAFWK | $B tail -15
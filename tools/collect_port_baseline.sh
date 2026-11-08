#!/busybox sh
# 只读采集显示、GPU 和容器依赖状态
B=/busybox
R=/hmos-root
echo DIPPER_PORT_BASELINE_BEGIN
$B uname -r
$B chroot "$R" /system/bin/param get bootevent.boot.completed
for p in /sys/class/drm/card*-DSI-*/modes /sys/class/drm/card*-DSI-*/status \
 /sys/class/kgsl/kgsl-3d0/gpu_model /sys/class/kgsl/kgsl-3d0/gpuclk \
 /sys/class/kgsl/kgsl-3d0/gpubusy \
 /sys/class/kgsl/kgsl-3d0/devfreq/available_frequencies; do
 if [ -r "$p" ]; then echo "PATH=$p"; $B head -30 "$p"; echo; fi
done
if [ -r /proc/config.gz ]; then
 $B zcat /proc/config.gz | $B grep -E \
 '(^CONFIG_(NAMESPACES|PID_NS|IPC_NS|USER_NS|UTS_NS|NET_NS|CGROUPS|MEMCG|SECCOMP|ANDROID_BINDER_IPC|ANDROID_BINDERFS|ASHMEM|KVM|VHOST|VIRTIO)=|^# CONFIG_(PID_NS|IPC_NS|USER_NS|KVM) is not set)'
else
 echo KERNEL_CONFIG_UNAVAILABLE
fi
for p in "$R/dev/kvm" "$R/dev/binder" "$R/dev/hwbinder" "$R/dev/vndbinder" \
 "$R/dev/hmos_binder" "$R/dev/dri/renderD128" "$R/dev/kgsl-3d0"; do
 if [ -e "$p" ]; then $B ls -l "$p"; else echo "MISSING=$p"; fi
done
$B ls -l /proc/self/ns
$B tail -12 "$R/data/local/tmp/dipper-composer-metrics.txt"
echo DIPPER_PORT_BASELINE_END

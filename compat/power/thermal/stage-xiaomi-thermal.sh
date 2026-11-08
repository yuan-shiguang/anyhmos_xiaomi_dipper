#!/busybox sh
set -eu

B=/busybox
C=/graphics/xiaomi-thermal
R=/hmos-root/data/local/tmp/dipper-xiaomi-thermal/root

# 先恢复板温采集与原厂温控接口
for module in qcom-vadc-common qcom-spmi-adc5-gen3 mi_thermal_interface; do
    name=$(printf '%s' "$module" | $B tr '-' '_')
    if [ ! -d "/sys/module/$name" ]; then
        $B insmod "/lib/modules/$module.ko"
    fi
done

# 为温控保留原厂目录结构，不覆盖鸿蒙的设备配置
$B mkdir -p "$R"
$B tar -xf "$C/thermal-root.tar" -C "$R"
$B mkdir -p "$R/sys" "$R/proc" "$R/dev" "$R/compat/audio-runtime"
$B mkdir -p "$R/data/vendor/thermal/config"
for path in sys proc dev compat/audio-runtime; do
    if ! $B mountpoint -q "$R/$path"; then
        $B mount --bind "/$path" "$R/$path"
    fi
done
$B cp "$C/native_thermal_launcher" /hmos-root/compat/bin/native_thermal_launcher
$B chmod 0755 /hmos-root/compat/bin/native_thermal_launcher

# 运行环境就绪后交给初始化服务启动原厂温控
$B chroot /hmos-root /system/bin/param set dipper.debug.start.thermal true

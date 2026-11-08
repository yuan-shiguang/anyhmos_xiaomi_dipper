#!/busybox sh
# 米 8（dipper）Wi-Fi：sdm845 用 qcacld-3.0 + icnss，官方 4.9 内核把驱动编进内核
# （CONFIG_QCA_CLD_WLAN=y、CONFIG_ICNSS=y、CONFIG_CNSS_GENL=y）。
# 注意：qcacld-2.0 是 msm8974/8994 时代的 WCNSS 驱动，不适用于 sdm845。
# 驱动内置时只等 netdev；模块化内核才从暂存目录补 insmod。
set -eu
B=/busybox
R=/hmos-root
stage=check
trap 'code=$?; echo "WIFI_START_EXIT=$code STAGE=$stage"' EXIT
$B cat /proc/sys/kernel/random/boot_id /proc/uptime

netdev_ready() { [ -e /sys/class/net/wlan0 ]; }

stage=wait
i=0
while ! netdev_ready; do
    [ "$i" -lt 10 ] || break
    $B sleep 1; i=$((i+1))
done

if ! netdev_ready; then
    stage=driver
    for ko in wlan.ko qca_cld3_wlan.ko; do
        for d in $R/compat/modules $R/vendor_dlkm/lib/modules $R/vendor/lib/modules; do
            if [ -f "$d/$ko" ]; then
                echo "WIFI_MODULE_LOAD=$d/$ko"
                $B insmod "$d/$ko" || echo "WIFI_MODULE_FAILED=$ko"
            fi
        done
    done
    # 校准就绪节点随平台不同，存在才写
    for n in /sys/kernel/cnss/fs_ready /sys/kernel/icnss/fs_ready; do
        if [ -e "$n" ]; then
            echo 1 > "$n"
            echo "WIFI_CALIBRATION_READY=$n"
        fi
    done
    i=0
    while ! netdev_ready; do
        [ "$i" -lt 30 ] || break
        $B sleep 1; i=$((i+1))
    done
fi

if ! netdev_ready; then
    # 米 8 的 WLAN 固件（bdwlan/wlan_mac 等）在设备自己的 vendor 分区，
    # 打包的 vendor 来自供体，缺固件时这里如实标记而不是假装成功。
    echo WIFI_NETDEV_ABSENT_SDM845=1
    exit 0
fi
echo WIFI_KERNEL_NETDEV_READY=1
stage=native-services
$B chroot "$R" /system/bin/param set dipper.debug.start.wifi true
echo WIFI_NATIVE_SERVICES_REQUESTED=1
stage=complete

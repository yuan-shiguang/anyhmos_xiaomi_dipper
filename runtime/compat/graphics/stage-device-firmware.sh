#!/busybox sh
# 米 8（dipper）设备固件定位：让内核能取到 WLAN 校准文件。
#
# qcacld-3.0 用 request_firmware 取 wlan/qca_cld/WCNSS_qcom_cfg.ini 与 wlan_mac.bin，
# 而兼容层的根是 initramfs，没有 /lib/firmware，所以必须显式告诉 firmware_class 去哪找。
# 首选设备自己 vendor 分区里的固件（4.9 内核 + 原厂校准），只设置路径、不做拷贝；
# 只有 vendor 里没有时才回退到 ramdisk 载荷（build 时从 vendor.img 抽出）。
set -eu
B=/busybox
R=/hmos-root
stage=check
trap 'code=$?; echo "DEVICE_FIRMWARE_EXIT=$code STAGE=$stage"' EXIT

payload=
for d in /graphics/dipper-firmware "$R/compat/dipper-firmware" /dipper-firmware; do
    if [ -d "$d" ]; then payload=$d; break; fi
done
if [ -n "$payload" ]; then
    echo "DEVICE_FIRMWARE_PAYLOAD=$payload"
else
    echo DEVICE_FIRMWARE_PAYLOAD_ABSENT=1
fi

nonempty() {
    [ -d "$1" ] && [ -n "$($B ls "$1" 2>/dev/null)" ]
}

stage=wlan
fwroot=
if nonempty "$R/vendor/firmware/wlan/qca_cld"; then
    fwroot=$R/vendor/firmware
    echo WLAN_FIRMWARE_SOURCE=device-vendor
elif [ -n "$payload" ] && nonempty "$payload/qca_cld"; then
    $B mkdir -p "$R/compat/firmware/wlan/qca_cld"
    $B cp -f "$payload"/qca_cld/* "$R/compat/firmware/wlan/qca_cld/"
    $B chmod 0644 "$R/compat/firmware/wlan/qca_cld"/*
    fwroot=$R/compat/firmware
    echo WLAN_FIRMWARE_SOURCE=payload
fi
if [ -n "$fwroot" ]; then
    echo "WLAN_FIRMWARE_FILES=$($B ls "$fwroot/wlan/qca_cld" | $B wc -l)"
    # 内核按 <path>/<request_firmware 的名字> 查找，即 <path>/wlan/qca_cld/*
    fwpath=/sys/module/firmware_class/parameters/path
    if [ -w "$fwpath" ]; then
        echo "$fwroot" > "$fwpath"
        echo "FIRMWARE_CLASS_PATH=$($B cat "$fwpath")"
        # 内置驱动在 initramfs 用户态之前就 probe 过一次，这里写的路径只对
        # 模块化加载或重新 probe 生效；内置场景还需要在 kernel cmdline 上带
        # firmware_class.path=/vendor/firmware，否则这次 probe 取不到校准文件。
        echo FIRMWARE_CLASS_PATH_RUNTIME_ONLY=1
    else
        echo FIRMWARE_CLASS_PATH_READONLY=1
    fi
else
    echo WLAN_FIRMWARE_ABSENT=1
fi

# 设备自带的 WLAN 模块（按 4.9 内核编译）。start-wifi 会先看 vendor 目录，
# 这里只在载荷存在时补一份到 /compat/modules，供 vendor 缺模块的内核使用。
stage=modules
if [ -n "$payload" ] && nonempty "$payload/modules"; then
    $B mkdir -p "$R/compat/modules"
    for ko in "$payload"/modules/*.ko; do
        [ -f "$ko" ] || continue
        $B cp -f "$ko" "$R/compat/modules/"
    done
    $B chmod 0644 "$R/compat/modules"/*.ko 2>/dev/null || true
    echo "DEVICE_MODULES_STAGED=$($B ls "$R/compat/modules"/*.ko 2>/dev/null | $B wc -l)"
fi

stage=complete

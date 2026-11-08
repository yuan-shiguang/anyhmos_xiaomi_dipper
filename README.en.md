# Xiaomi Mi 8 (dipper) · HarmonyOS NEXT

[简体中文](README.md) · [Building](docs/BUILDING.md) · [Source map](docs/SOURCE_MAP.md) · [Testing](docs/VERIFICATION.md) · [Provenance and licensing](docs/PROVENANCE.md)

A HarmonyOS NEXT port for the Xiaomi Mi 8 (**dipper**). A compatibility layer connects HarmonyOS to the phone's display, touch input, audio and wireless hardware. The port is still in progress: the code is organised around this device's parameters, and hardware results are only valid where the [test records](docs/VERIFICATION.md) say so.

Other models can also borrow parts of the code and the approach. The general recipe is to pack Huawei's system, vendor, sys_prod and your own device's vendor_dlkm into super and flash it, then take your device's boot, vendor_boot and init_boot, change the mount points, bring up a USB serial console and fix the boot from the logs. The Mi 8 cannot take that route: it has no `super`, no `vendor_boot` and no `init_boot`, its Android partitions are single-slot and static, and only the bootloader chain is A/B. So here the HarmonyOS system image goes straight into the 3 GiB `system` partition, vendor into the 1 GiB `vendor` partition, and the compatibility payload lives on the 832 MiB `cust` partition, mounted at boot from `/cust/compat` to `/compat`. See [partition layout](docs/DEVICE-PARTITIONS.md) for the measured sizes and the reasoning.

Device parameters used here: Snapdragon 845 (sdm845, Adreno 630), 6.21-inch AMOLED 1080×2248 at 60Hz over a single DSI link, 3400mAh battery with 18W charging and no wireless charging, Wi-Fi 5 (802.11ac, 2.4/5GHz, 80MHz), Bluetooth 5.0, a single bottom speaker plus earpiece, USB-C audio with no 3.5mm jack, and a rear fingerprint sensor.

This repository contains the component sources currently in use, startup configuration and build tools. Several features still need work; the table below describes their current status.

## Feature status

The table describes what the compatibility layer implements today and which problems are known. Hardware acceptance is tracked in the [test records](docs/VERIFICATION.md); anything not re-checked on the device counts as unfinished.

| Feature | Working today | Remaining work |
| --- | --- | --- |
| Boot and desktop | The compatibility layer can take over the boot flow, reach the desktop, open windows and take system screenshots. | Boot takes a long time. Some applications and services still stall or crash, and long-term stability needs improvement. |
| Display | The display backend initialises a 1080×2248 single-DSI panel and picks single- or dual-plane scanout from the planes the connector actually exposes. | Screen on/off, suspend and resume, and extended use need device validation; bars and white screens still need investigation. |
| GPU and refresh rate | Mesa/Turnip/Zink graphics support is integrated. The display runs at 1080×2248 and 60Hz, using the kernel's native GPU governor. | The panel is 60Hz only, so high refresh rates do not apply. Graphics performance, format support and synchronization need improvement. |
| Touch and responsiveness | The stock kernel's built-in ST FTS (`fts@49`) / mXT640U driver reports standard multitouch; the compatibility layer only hands the evdev node to the input service, with no vendor algorithm bridge. | Sampling, multitouch, edge input and resume behavior need device verification. |
| Brightness | Manual brightness adjustment works. | Automatic brightness, minimum brightness and resume behavior need testing. |
| Power and volume buttons | A short power press turns the screen off or on; a long press opens the shutdown menu. Volume buttons are connected to the system. | Button behavior across lock, suspend and repeated interactions needs broader testing. Deep-sleep power consumption has not been evaluated. |
| Wi-Fi | Uses the stock kernel built-in qcacld-3.0 driver (`CONFIG_QCA_CLD_WLAN=y` + icnss), not qcacld-2.0. The compatibility layer points `firmware_class.path` at `firmware/wlan/qca_cld` inside the device vendor partition and copies nothing; the ramdisk payload is only a fallback when vendor lacks those files. | The Mi 8 WLAN calibration files must really exist in the vendor partition or `wlan0` never appears. A built-in driver has already probed before user space starts, so the runtime path only helps modular loading or a re-probe; the built-in case also needs `firmware_class.path=/vendor/firmware` on the kernel command line. MAC address, 5GHz, power and long-running association still need on-device checks. |
| Wi-Fi generation display | The status bar can show the Wi-Fi 5 badge for the actual connection. | The device has no Wi-Fi 6/7, so those badges must never appear; this needs a regression check. |
| Bluetooth | Partially working: basic toggling and nearby-device discovery are implemented. | Enabling Bluetooth can temporarily freeze the system and restart the system interface. Pairing, headset playback and calls, keyboards, mice and BLE are not fully tested. The device does not support NearLink. |
| Speakers and audio | Playback now uses an sdm845 tinyalsa backend: a statically linked `dipper-tinyalsa-server` drives the kernel PCM nodes directly and opens the `SLIMBUS_0_RX` downlink route; volume is applied as Q15 fixed-point scaling in user space. | The Mi 8 has no `/dev/agm`, so the sm8550 PAL/AGM backend cannot work here. Audible output, the TAS2557 amplifier Program/Configuration (normally pushed by the vendor audio HAL) and the capture uplink route all still need verification on the device. |
| Video and images | Some software decoding paths work. Selected system and third-party applications display video and images. | Hardware video decoding is unfinished. Some applications and formats still fail or stutter. |
| Keyboard | The keyboard appears and accepts input, including Wi-Fi passwords. | More applications, web input fields and languages need testing. |
| Third-party and preload applications | Preload installation is integrated. Some startup blocks showing "application data is being decrypted" have been fixed; applications including Douyin can open and display content. | Some applications remain on their splash screen or crash. Login and features that depend on web pages still have problems. |
| User data | The entire userdata partition mounts directly as `/data`, retaining the setup wizard. The updated code accepts normally formatted ext4 without a specific UUID or a manually created marker. | The updated mount logic has passed isolated image tests; a fresh installation on the device remains to be tested. Upgrade and rollback procedures need further work. |
| Battery and charging status | Real battery and charging readings are available. The system no longer shows a constant 100%. | Charging-state changes, thermal behavior, 18W fast charging, battery life and standby drain need testing. |
| Time synchronization | Network time synchronization works. | Offline time, time zones and time displayed in some parts of the interface remain inconsistent. |
| USB debugging | Tools provide command execution, file transfer and screenshots. | Communication can still stop after rebooting or USB state changes. Connection reliability needs improvement. |
| A/B boot | The system can mark a boot successful after the desktop is ready. | Full OTA and A/B update support is unfinished. |
| Web pages and WebView | Not working correctly yet. | Some pages fail to open or remain blank. Douyin web login can remain on a loading screen. |
| Security and encryption | Software compatibility implementations support selected services and applications. | TEE, hardware-backed keys, lock credentials and HUKS are incomplete. Stock-equivalent security is unavailable; encrypted apps from the app market can't be decrypted for use; payment and DRM features are unverified. |
| Camera, microphone, sensors and fingerprint | Unfinished. | Camera capture, recording, sensors and the rear fingerprint sensor need individual adaptation and testing. |
| Cross-device features | Unfinished. | Device discovery, connections and cross-device functionality need further adaptation. |

## Changes to each partition

| Partition | Changes |
| --- | --- |
| `boot` | Uses the 4.9 official sdm845 kernel with the interfaces HarmonyOS IPC needs; hardware parameters are checked against the device's 4.9 official kernel sources. The partition is 64 MiB and about 21 MiB are free, so the compatibility layer does not fit here. |
| `system` | Receives the HarmonyOS system image and is mounted at boot as a static ext4 partition via `by-name`, with no LP metadata and no dm-linear. The partition is 3072 MiB; the builder checks the size and fails instead of truncating. |
| `vendor` | Receives the HarmonyOS vendor image, mounted the same way. The partition is 1024 MiB. |
| `cust` | Holds the compatibility payload (source interfaces, configuration, audio and firmware files). It is mounted at `/cust` and `/cust/compat` is bound to `/compat`. At 832 MiB it is the only free static partition in this package. |

## Source layout

- `compat/`: component sources, interfaces and shared dependencies.
- `runtime/`: startup scripts and runtime configuration.
- `patches/donor/`: patch recipes for original HarmonyOS libraries, with file hashes and checks at each modification point.
- `external/`: upstream source information, kernel and Mesa patches, and licenses.
- `tools/`: build and source verification tools.
- `output/`: host scripts for debugging, transfers, screenshots, backups and recovery.
- `tests/`, `analysis/` and `evidence/`: test cases and records with private information removed.

Each component retains the implementation currently in use and its required dependencies. See the [source map](docs/SOURCE_MAP.md) for details, and `SOURCE-SELECTION.json`, `SOURCE-MANIFEST.json` and `SHA256SUMS` for file inventories and checksums.

## Building and use

Builds use WSL. Follow the [build instructions](docs/BUILDING.md) to prepare the SDK, upstream sources and link dependencies, then list the available targets:

```sh
python3 tools/build_current.py --list
```

UserIAM components are built with `tools/build_useriam.py`. The repository provides build steps for the compatibility layer and related components. A complete ROM also requires separately prepared system images, firmware and applications, which are not distributed here.

Use the tools under `output/` for the system's USB debug connection, running commands sequentially. Before flashing, back up this device's `boot`, `system`, `vendor` and `cust` partitions and check the device model and image checksums. See the [testing documentation](docs/VERIFICATION.md) for test coverage and records.

For a fresh installation, format userdata as ext4 in Recovery first. The bootstrap mounts the whole partition and creates the required directories. No special UUID or initialization file is needed, and it never formats automatically. Erasing the partition without creating a filesystem is insufficient. See the [userdata instructions](docs/USERDATA.md).

## Reporting issues and credits

Include the commit used, reproduction steps and logs with private information removed. Do not submit passwords or tokens.

This port draws on [AnyHarmonyOS](https://github.com/YiKongk/AnyHarmonyOS) and other projects. See [provenance and licensing](docs/PROVENANCE.md) for source attribution and modifications. This is a community port, unaffiliated with Xiaomi, Huawei or the upstream projects' official device support.

import argparse
import hashlib
import os
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "compat/sensors"


def dependency(parser, name, description):
    variable = "SENSORS_" + name.upper().replace("-", "_")
    default = os.environ.get(variable)
    parser.add_argument("--" + name, type=Path, default=default,
                        required=default is None, help=f"{description} ({variable})")


def main():
    parser = argparse.ArgumentParser(description="Build the dipper SSC and sensor HDI services")
    dependency(parser, "sdk", "OHOS native SDK directory")
    dependency(parser, "generated", "Official generated sensor HDI headers")
    dependency(parser, "hdf", "drivers_hdf_core/interfaces/inner_api directory")
    dependency(parser, "public-headers", "Matching OHOS platform headers")
    dependency(parser, "ipc-headers", "Matching iremote_broker.h and peer_holder.h directory")
    dependency(parser, "registry-headers", "Matching service manager headers")
    dependency(parser, "donor-libs", "Original platform link libraries")
    dependency(parser, "system-libs", "Original libc.so and libhilog.so directory")
    dependency(parser, "begetutil", "Original libbegetutil.z.so path")
    parser.add_argument("--output", type=Path, default=ROOT / "out/sensors")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    suffix = ".exe" if os.name == "nt" else ""
    tools = args.sdk / "llvm/bin"
    cc = str(tools / ("clang" + suffix))
    cxx = str(tools / ("clang++" + suffix))
    qrtr = ROOT / "compat/audio/pd-mapper"
    common = ["--target=aarch64-linux-ohos", "-O2", "-Wall", "-Wextra", "-Werror"]
    cflags = [*common, "-std=c11", "-D_GNU_SOURCE", "-fPIC", "-pthread", "-I" + str(qrtr)]

    def run(command):
        subprocess.run([str(value) for value in command], check=True)

    # 后端复用现有QRTR源码，静态链接到HDI实现
    objects = []
    for source in (SOURCE / "ssc_backend.c", qrtr / "qrtr.c", qrtr / "logging.c"):
        obj = output / (source.stem + ".o")
        run([cc, *cflags, "-c", source, "-o", obj])
        objects.append(obj)
    archive = output / "libssc_backend.a"
    run([tools / ("llvm-ar" + suffix), "rcs", archive, *objects])
    run([cc, *cflags, "-shared", *objects, "-Wl,-soname,libssc_backend.so",
         "-o", output / "libssc_backend.so"])
    time_object = output / "ssc_time.o"
    run([cc, *cflags, "-c", SOURCE / "ssc_time.c", "-o", time_object])

    # 官方生成头和原厂链接库由调用方显式提供
    includes = [SOURCE, args.generated, args.generated / "sensor", args.public_headers,
                args.hdf / "hdi", args.hdf / "utils", args.hdf / "osal/uhdf",
                args.ipc_headers, args.hdf / "ipc"]
    cppflags = [*common, "-std=c++17", "-g", "-fPIC", "-fvisibility=hidden", "-fno-rtti",
                "-flto", "-fsanitize=cfi", "-fsanitize-cfi-cross-dso",
                "-mbranch-protection=pac-ret", "-fstack-protector-strong",
                *["-I" + str(path) for path in includes]]
    base_libs = [args.donor_libs / name for name in
                 ("libutils.z.so", "libc++.so", "libipc_single.z.so", "libipc_common.z.so")]
    system_libs = [args.system_libs / name for name in ("libc.so", "libhilog.so")]
    service = output / "libsensor_interface_service_3.1.z.so"
    run([cxx, *cppflags, "-shared", "-Wl,--no-undefined", "-Wl,-z,relro,-z,now",
         "-Wl,-soname," + service.name, SOURCE / "sensor_interface_service.cpp",
         archive, time_object, *base_libs, *system_libs, "-o", service])

    publisher_includes = [args.registry_headers, args.hdf / "ipc", args.hdf / "hdi/base",
                          args.hdf / "core"]
    linker = "-Wl,--dynamic-linker=/system/lib/ld-musl-aarch64.so.1"
    run([cxx, *["-I" + str(path) for path in publisher_includes], *cppflags,
         "-include", ROOT / "compat/platform/platform_cpp_config.h", "-fno-exceptions",
         "-nostdlib++", "-Wl,--no-undefined", linker,
         "-Wl,-rpath,/system/lib64:/system/lib64/chipset-sdk:/system/lib64/chipset-sdk-sp:/system/lib64/platformsdk",
         SOURCE / "sensor_hdi_publisher.cpp", args.donor_libs / "libhdi.z.so",
         args.donor_libs / "libhdi_base.z.so", *base_libs, *system_libs,
         args.begetutil, "-o", output / "sensor_hdi_publisher"])
    run([cc, *common, "-std=c11", SOURCE / "native_sensorspd_launcher.c",
         linker, "-o", output / "native_sensorspd_launcher"])
    for name in ("libssc_backend.a", "libssc_backend.so", service.name,
                 "sensor_hdi_publisher", "native_sensorspd_launcher"):
        artifact = output / name
        print(hashlib.sha256(artifact.read_bytes()).hexdigest(), artifact)


if __name__ == "__main__":
    main()

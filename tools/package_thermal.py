"""Prepare the stock thermal payload from external extracted dipper assets.

The input directory contains mi_thermald, odm/etc/*.conf and vendor/etc/*.conf.
Compile native_thermal_launcher.c with the OHOS SDK and pass that executable.
The output belongs at graphics/xiaomi-thermal in the compatibility ramdisk.
Copy the two init configs to /compat/etc before native init parses its imports;
import both configs from the existing private-persist init configuration.
Invoke stage-xiaomi-thermal.sh after the audio runtime and modules are ready.
The stage reconstructs its runtime on every boot, including an empty userdata.
This tool only assembles payloads and does not flash or enable charging modes.
"""

import argparse
import io
import shutil
import tarfile
from pathlib import Path


def package(stock, launcher, output):
    source = Path(__file__).resolve().parents[1] / 'compat/power/thermal'
    output.mkdir(parents=True, exist_ok=True)
    inputs = [('bin/mi_thermald', stock / 'mi_thermald', 0o755)]
    # 配置保持原厂内容与目录，不重新制定充电温控阈值
    for directory in ('odm/etc', 'vendor/etc'):
        configs = sorted((stock / directory).glob('*.conf'))
        if not configs:
            raise FileNotFoundError(stock / directory)
        inputs.extend((directory + '/' + p.name, p, 0o644) for p in configs)
    with tarfile.open(output / 'thermal-root.tar', 'w') as archive:
        for name, path, mode in inputs:
            data = path.read_bytes()
            member = tarfile.TarInfo(name)
            member.mode = mode
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    shutil.copyfile(launcher, output / 'native_thermal_launcher')
    shutil.copyfile(source / 'stage-xiaomi-thermal.sh', output / 'stage-xiaomi-thermal.sh')
    # 初始化配置随镜像装配，服务不依赖上次运行留下的手工设置
    for name in ('init-xiaomi-thermal.cfg', 'init-fusion-services-r2.cfg'):
        shutil.copyfile(source / name, output.parent / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock-root', type=Path, required=True)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    package(args.stock_root, args.launcher, args.output)


if __name__ == '__main__':
    main()

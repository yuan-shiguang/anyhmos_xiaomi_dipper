"""Assemble host flashing tools around the verified private image build."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--package', type=Path, required=True)
parser.add_argument('--platform-tools', type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
package = args.package
report = json.loads((package / 'reports/image-build.json').read_text(encoding='utf-8'))
if not report['userdata_readback_verified'] or not report['super_readback_verified']:
    raise ValueError('Images have not passed readback checks')
bin_dir = package / 'bin'
bin_dir.mkdir(exist_ok=True)
for name in [
    'fastboot.exe',
    'AdbWinApi.dll',
    'AdbWinUsbApi.dll',
    'libwinpthread-1.dll',
    'NOTICE.txt',
    'source.properties',
]:
    shutil.copyfile(args.platform_tools / name, bin_dir / name)
shutil.copyfile(repo / 'tools/Flash-Dipper.ps1', package / 'Flash-Dipper.ps1')
for name, mode in [('Verify-Package.bat', 'Verify'), ('FlashScript.bat', 'Flash')]:
    (package / name).write_text(
        '@echo off\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Flash-Dipper.ps1" -Mode '
        + mode
        + '\r\nif errorlevel 1 echo Operation failed. Check the message above.\r\npause\r\n',
        newline='',
        encoding='utf-8',
    )
# 重新计算包内文件校验和，排除运行日志
files = []
for path in sorted(package.rglob('*')):
    relative = path.relative_to(package)
    if (
        not path.is_file()
        or relative.parts[0] == 'logs'
        or path.name in {'PACKAGE.json', 'SHA256SUMS'}
    ):
        continue
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    files.append({'path': relative.as_posix(), 'bytes': path.stat().st_size, 'sha256': digest})
for artifact in report['artifacts']:
    if artifact not in files:
        raise ValueError('Image changed since readback: ' + artifact['path'])
runtime_path = package / 'reports/device-validation.json'
boot_validated = False
remaining = ['Zhuoyitong runtime', 'all application GPU paths']
if runtime_path.exists():
    runtime = json.loads(runtime_path.read_text(encoding='utf-8'))
    vendor_hash = next(r['sha256'] for r in files if r['path'] == 'images/vendor_boot.img')
    boot_validated = (
        runtime.get('vendor_boot_sha256') == vendor_hash
        and runtime.get('entered_setup_or_desktop') is True
        and runtime.get('autostart_complete') is True
        and runtime.get('bootcontrol_successful') is True
    )
    # 仅采用与当前启动镜像匹配的实机验证结果
manifest = {
    'kind': 'dipper-experimental',
    'donor': report['donor'],
    'device_boot_tested': boot_validated,
    'userdata_bytes': report['userdata_bytes'],
    'super_bytes': report['super_bytes'],
    'target_slot': 'a',
    'data_wipe_required': True,
    'contains_userdata_seed': True,
    'not_validated': remaining,
    'files': files,
}
(package / 'PACKAGE.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
with (package / 'PACKAGE.json').open('rb') as stream:
    manifest_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
checksums = ''.join(f'{r["sha256"]}  {r["path"]}\n' for r in files)
(package / 'SHA256SUMS').write_text(
    checksums + manifest_hash + '  PACKAGE.json\n', encoding='utf-8'
)
print(
    json.dumps(
        {
            'package': str(package),
            'files': len(files) + 2,
            'bytes': sum(r['bytes'] for r in files),
            'device_flashed': False,
        },
        indent=2,
    )
)

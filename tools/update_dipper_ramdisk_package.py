"""Update only host vendor_boot artifacts, then rebuild package checksums."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--donor', type=Path, required=True)
parser.add_argument('--package', type=Path, required=True)
parser.add_argument('--platform-tools', type=Path, required=True)
args = parser.parse_args()
work = args.donor / 'port-work'
payload = json.loads((work / 'payload-integration-report.json').read_text(encoding='utf-8'))
image = work / 'vendor_boot_dipper_111sp12.img'
with image.open('rb') as stream:
    digest = hashlib.file_digest(stream, 'sha256').hexdigest()
# 核对归档不变量后更新宿主机上的启动镜像
if digest != payload['vendor_boot_sha256'] or image.stat().st_size != payload['vendor_boot_bytes']:
    raise ValueError('Ramdisk build report mismatch')
if not all(
    payload[key] for key in ['cpio_roundtrip_verified', 'kernel_unchanged', 'dtb_unchanged']
):
    raise ValueError('Ramdisk invariants not verified')
report_path = args.package / 'reports/image-build.json'
report = json.loads(report_path.read_text(encoding='utf-8'))
artifact = next(r for r in report['artifacts'] if r['path'] == 'images/vendor_boot.img')
if artifact['bytes'] != image.stat().st_size:
    raise ValueError('Unexpected vendor_boot capacity change')
shutil.copyfile(image, args.package / 'images/vendor_boot.img')
shutil.copyfile(
    work / 'payload-integration-report.json', args.package / 'reports/payload-integration.json'
)
artifact['sha256'] = digest
report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
subprocess.run(
    [
        sys.executable,
        str(Path(__file__).with_name('assemble_dipper_package.py')),
        '--package',
        str(args.package),
        '--platform-tools',
        str(args.platform_tools),
    ],
    check=True,
)
print(
    json.dumps(
        {'vendor_boot_sha256': digest, 'other_images_rebuilt': False, 'device_flashed': False}
    )
)

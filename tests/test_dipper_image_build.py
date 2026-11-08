"""Build a synthetic dipper image set and check the builder's refusals.

Run with: python3 -m unittest tests.test_dipper_image_build

The donor and boot images here are fixtures, not the real ones: what is under test
is the capacity accounting, the ext4 cust packing and the header checks. A layout
with small capacities keeps the test fast; the real table is covered by
tests/test_dipper_layout.py.
"""

import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILDER = ROOT / 'tools/build_dipper_images.py'
MIB = 1024 * 1024


def run(*args):
    return subprocess.run(args, text=True, capture_output=True)


def make_ext4(path, megabytes):
    with path.open('wb') as stream:
        stream.truncate(megabytes * MIB)
    result = run('mke2fs', '-t', 'ext4', '-F', '-q', str(path))
    if result.returncode:
        raise unittest.SkipTest('mke2fs is required for this test: ' + result.stderr)


def make_boot(path, kernel=64 * 1024, ramdisk=32 * 1024, version=1, page=4096):
    header = struct.pack('<8s10I', b'ANDROID!', kernel, 0x8000, ramdisk, 0x1000000,
                         0, 0, 0x100, page, version, 0)
    header += b'\0' * (page - len(header))
    payload = b'K' * kernel + b'R' * ramdisk
    payload += b'\0' * ((-len(payload)) % page)
    path.write_bytes(header + payload)


def layout(capacities, extra=None):
    partitions = []
    for name, megabytes in capacities.items():
        partitions.append({'label': name, 'size_kb': megabytes * 1024,
                           'size_bytes': megabytes * MIB, 'slot_model': 'single'})
    for name in extra or ():
        partitions.append({'label': name, 'size_kb': 1024, 'size_bytes': MIB})
    absent = ['super', 'vendor_boot', 'init_boot', 'system_ext', 'product', 'odm',
              'vendor_dlkm', 'metadata', 'vbmeta_system']
    if extra:
        absent = [name for name in absent if name not in extra]
    return {'slot_model': 'single', 'partitions': partitions, 'absent': absent}


class DipperImageBuildTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='dipper-images-')
        self.work = Path(self.directory.name)
        self.donor = self.work / 'donor/extracted/base'
        self.donor.mkdir(parents=True)
        make_ext4(self.donor / 'system.img', 8)
        make_ext4(self.donor / 'vendor.img', 4)
        self.boot = self.work / 'boot.img'
        make_boot(self.boot)
        payload = self.work / 'payload'
        (payload / 'compat/bin').mkdir(parents=True)
        (payload / 'compat/bin/init').write_text('init\n')
        (payload / 'hmos/sys_prod/etc').mkdir(parents=True)
        (payload / 'hmos/sys_prod/etc/hmos.para').write_text('sys_prod\n')
        self.payload = payload
        self.layout = self.work / 'layout.json'
        self.layout.write_text(json.dumps(layout(
            {'system': 32, 'vendor': 16, 'cust': 8, 'boot': 4})))

    def tearDown(self):
        self.directory.cleanup()

    def build(self, output='out', layout=None, extra=()):
        return run(sys.executable, str(BUILDER),
                   '--donor', str(self.work / 'donor'),
                   '--boot', str(self.boot),
                   '--payload', str(self.payload),
                   '--output', str(self.work / output),
                   '--work', str(self.work / ('work-' + output)),
                   '--layout', str(layout or self.layout), *extra)

    def test_builds_four_images_and_records_capacities(self):
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        images = self.work / 'out/images'
        self.assertEqual(sorted(p.name for p in images.iterdir()),
                         ['boot.img', 'cust.img', 'system.img', 'vendor.img'])
        report = json.loads((self.work / 'out/reports/image-build.json').read_text())
        self.assertEqual(report['partition_model'], 'single-slot static')
        self.assertFalse(report['device_flashed'])
        self.assertTrue(report['verified_after_write'])
        by_path = {a['path']: a for a in report['artifacts']}
        self.assertEqual(by_path['images/system.img']['filesystem'], 'ext4')
        self.assertEqual(by_path['images/system.img']['capacity_bytes'], 32 * MIB)
        self.assertEqual(by_path['images/cust.img']['bytes'], 8 * MIB)
        self.assertEqual(by_path['images/cust.img']['format'], 'raw-ext4')
        self.assertEqual(by_path['images/cust.img']['staged_partitions'], 'sys_prod/preload')
        self.assertEqual(report['boot']['header_version'], 1)
        # cust 必须是能挂的 ext4，并且真的装进了载荷
        self.assertEqual((images / 'cust.img').read_bytes()[1080:1082], b'\x53\xef')

    def test_refuses_a_payload_larger_than_the_partition(self):
        self.layout.write_text(json.dumps(layout(
            {'system': 1, 'vendor': 16, 'cust': 8, 'boot': 4})))
        result = self.build()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Nothing was truncated', result.stdout + result.stderr)
        self.assertIn('--system-bytes', result.stdout + result.stderr)
        self.assertFalse((self.work / 'out/images/system.img').exists())

    def test_refuses_a_dynamic_partition_layout(self):
        dynamic = self.work / 'dynamic.json'
        dynamic.write_text(json.dumps(layout(
            {'system': 32, 'vendor': 16, 'cust': 8, 'boot': 4}, extra=['super'])))
        result = self.build(layout=dynamic)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('super', result.stdout + result.stderr)
        self.assertIn('DEVICE-PARTITIONS.md', result.stdout + result.stderr)

    def test_refuses_a_boot_image_that_is_not_v1_page_4096(self):
        make_boot(self.boot, version=2)
        result = self.build()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('v1 page 4096', result.stdout + result.stderr)

    def test_grown_system_partition_is_accepted_explicitly(self):
        self.layout.write_text(json.dumps(layout(
            {'system': 1, 'vendor': 16, 'cust': 8, 'boot': 4})))
        result = self.build(extra=('--system-bytes', str(32 * MIB)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads((self.work / 'out/reports/image-build.json').read_text())
        record = next(a for a in report['artifacts'] if a['path'].endswith('system.img'))
        self.assertEqual(record['capacity_bytes'], 32 * MIB)


if __name__ == '__main__':
    unittest.main()

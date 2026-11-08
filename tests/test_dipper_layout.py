"""Check the recorded dipper partition layout matches what the tools assume.

The layout comes from the official MIUI V12.5.2.0.QEACNXM images/partition.xml.
dipper is a single-slot, static-partition device: there is no super, no
vendor_boot, no init_boot and no metadata. Tools that still assume a
dynamic-partition layout must fail loudly instead of emitting unflashable images.
"""

import importlib.util
import json
from pathlib import Path
import re
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
LAYOUT = ROOT / 'tools/dipper_partitions.json'
FLASH = ROOT / 'tools/Flash-Dipper.ps1'
BUILDER = ROOT / 'tools/build_dipper_images.py'

ABSENT = ('super', 'vendor_boot', 'init_boot', 'system_ext', 'product', 'odm',
          'vendor_dlkm', 'metadata', 'vbmeta_system')
BOOTLOADER_SLOTTED = {'abl', 'aop', 'cmnlib', 'cmnlib64', 'devcfg', 'hyp',
                      'keymaster', 'qupfw', 'storsec', 'tz', 'xbl', 'xbl_config'}
SIZES_MIB = {'system': 3072, 'vendor': 1024, 'cust': 832, 'boot': 64,
             'recovery': 64, 'userdata': 4096}


def load_builder():
    spec = importlib.util.spec_from_file_location('build_dipper_images', BUILDER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DipperLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.layout = json.loads(LAYOUT.read_text(encoding='utf-8'))
        cls.parts = {p['label']: p for p in cls.layout['partitions']}

    def test_android_partitions_are_present_and_single_slot(self):
        for name in ('boot', 'system', 'vendor', 'cust', 'modem', 'dsp',
                     'dtbo', 'vbmeta', 'userdata'):
            self.assertIn(name, self.parts, name)
            self.assertNotIn(name + '_a', self.parts, name + ' must not be slotted')
            self.assertNotIn(name + '_b', self.parts, name + ' must not be slotted')
        self.assertEqual(self.layout['slot_model'], 'single')

    def test_dynamic_partition_era_partitions_are_absent(self):
        for name in ABSENT:
            self.assertNotIn(name, self.parts, name)
        self.assertEqual(sorted(ABSENT), sorted(self.layout['absent']))

    def test_only_the_bootloader_chain_is_slotted(self):
        slotted = set()
        for label in self.parts:
            if label.endswith('_a') or label.endswith('_b'):
                slotted.add(label[:-2])
        self.assertEqual(slotted, BOOTLOADER_SLOTTED)
        self.assertEqual(sorted(BOOTLOADER_SLOTTED), self.layout['bootloader_slotted'])

    def test_partition_capacities_match_the_official_table(self):
        for name, mib in SIZES_MIB.items():
            self.assertEqual(self.parts[name]['size_bytes'], mib * 1024 * 1024, name)

    def test_builder_requires_the_static_layout(self):
        builder = load_builder()
        _, parts = builder.device_layout()
        # 官方分区表就是单槽静态布局：必须直接通过
        builder.assert_static_layout(parts)
        # 表里一旦出现 super/vendor_boot/init_boot，说明载入了别的机型的布局，必须拦停
        for extra in ('super', 'vendor_boot', 'init_boot'):
            with self.assertRaises(SystemExit) as caught:
                builder.assert_static_layout(dict(parts, **{extra: parts['system']}))
            message = str(caught.exception)
            self.assertIn(extra, message)
            self.assertIn('docs/DEVICE-PARTITIONS.md', message)

    def test_builder_refuses_to_truncate_an_oversized_payload(self):
        builder = load_builder()
        _, parts = builder.device_layout()
        declared = parts['system']['size_bytes']
        with self.assertRaises(SystemExit) as caught:
            builder.fit('system', declared + 4096, declared)
        message = str(caught.exception)
        self.assertIn('Nothing was truncated', message)
        self.assertIn('--system-bytes', message)
        builder.fit('system', declared, declared)

    def test_capacity_override_can_only_grow_a_partition(self):
        builder = load_builder()
        _, parts = builder.device_layout()
        declared = parts['system']['size_bytes']
        self.assertEqual(builder.capacity(parts, 'system', None), declared)
        grown = declared + 1024 ** 3
        self.assertEqual(builder.capacity(parts, 'system', grown), grown)
        for bad in (declared - 4096, declared + 512):
            with self.assertRaises(SystemExit):
                builder.capacity(parts, 'system', bad)

    def test_flasher_only_touches_partitions_that_exist(self):
        script = FLASH.read_text(encoding='utf-8-sig')
        probed = set(re.findall(r'partition-size:([a-zA-Z_]+)', script))
        # 分区名来自变量插值，这里只校验写死的那些
        for name in probed:
            self.assertIn(name, self.parts, name)
        images = set(re.findall(r"'([a-z_]+)\.img'", script))
        self.assertTrue(images, 'flasher must declare its image set')
        for name in images:
            self.assertIn(name, self.parts, name + '.img has no partition')
        # 米 8 上这些动作必须彻底消失
        for token in ('erase', 'set_active'):
            self.assertNotIn(
                "'" + token + "'", script,
                token + ' must not be issued on a static single-slot device',
            )


if __name__ == '__main__':
    unittest.main()

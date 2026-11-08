"""Check sensor packaging preserves native identities and real readiness ordering."""

import copy
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('package_sensors', ROOT / 'tools/package_sensors.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class SensorPackagingTests(unittest.TestCase):
    def test_native_identity_and_ready_order_in_existing_and_new_services(self):
        host = {'name': 'sensor_host', 'uid': 'sensor_host',
                'gid': ['sensor_host', 'oeminfo_nvme_server', 'vibrator_host'],
                'secon': 'u:r:sensor_host:s0', 'caps': ['SYS_NICE']}
        sensors = {'name': 'sensors', 'uid': 'sensor', 'gid': ['sensor', 'shell'],
                   'permission': ['original.sensor.permission'], 'critical': [1, 5, 10]}
        config = {'services': [{'name': 'keep', 'disabled': 1, 'permission': ['keep']}],
                  'jobs': [{'name': 'keep', 'cmds': ['keep original']} ]}
        before = copy.deepcopy(config)
        result = package.configure_services(config, host, sensors)
        self.assertEqual(config, before)
        self.assertEqual(result['services'][0], before['services'][0])
        selected = {s['name']: s for s in result['services']}
        for key in ('uid', 'gid', 'secon', 'caps'):
            self.assertEqual(selected['sensor_host'][key], host[key])
        for key in ('uid', 'gid', 'permission', 'critical'):
            self.assertEqual(selected['sensors'][key], sensors[key])
        for name in ('sensor_host', 'sensors'):
            self.assertIs(type(selected[name]['disabled']), int)
            self.assertEqual(selected[name]['disabled'], 1)
        jobs = {j.get('condition'): j['cmds'] for j in result['jobs']}
        self.assertNotIn('start sensors', jobs['dipper.debug.start.vibrator=true'])
        self.assertEqual(jobs[package.READY + '=true && dipper.debug.start.vibrator=true'],
                         ['start sensors'])
        self.assertEqual(jobs[package.READY + '=false'], ['stop sensors'])
        self.assertEqual(jobs['dipper.debug.start.vibrator=false'],
                         ['stop sensors', 'stop sensor_host'])
        repeated = package.configure_services(result, host, sensors)
        self.assertEqual(repeated, result)

    def test_audio_hook_runs_after_card_check_without_restarting_audio(self):
        anchor = 'AUDIO_CARD_CHECKED=1\n'
        original = ('keep before\n' + anchor +
                    '# ADSP首次就绪后异步采集传感器冷启动，不等待试验结束\n'
                    '$B setsid $B sh /dipper-sensors-cold-r1/start-sensors-cold.sh </dev/null &\n'
                    '$B sh "$S/stage-audio-nodes.sh"\nkeep after\n')
        result = package.audio_startup(original)
        self.assertNotIn('start-sensors-cold.sh', result)
        self.assertEqual(result.count('/dipper-sensors/start-sensors.sh'), 1)
        self.assertLess(result.index(anchor), result.index('SENSOR_STARTUP_PACKAGE='))
        self.assertLess(result.index('SENSOR_STARTUP_PACKAGE='), result.index('stage-audio-nodes.sh'))
        self.assertTrue(result.endswith('keep after\n'))
        self.assertNotIn('sleep', result)

    def test_parcel_patch_reorders_only_three_instruction_destinations(self):
        original = bytearray(0x128B0)
        for offset, before, _ in package.PARCEL_PATCHES:
            original[offset:offset + 4] = before
        result = package.patch_sensor_parcel(bytes(original))
        changes = {i for i, (a, b) in enumerate(zip(original, result)) if a != b}
        self.assertEqual(len(changes), 5)
        self.assertEqual([((int.from_bytes(result[o:o + 4], 'little') >> 10) & 0xFFF)
                          for o, _, _ in package.PARCEL_PATCHES], [120, 124, 128])
        original[package.PARCEL_PATCHES[0][0]] ^= 1
        with self.assertRaisesRegex(ValueError, 'instruction differs'):
            package.patch_sensor_parcel(bytes(original))

    def test_runtime_stage_preserves_other_commands_and_has_only_exact_dac(self):
        original = '#!/busybox sh\nset -eu\nR=/hmos-root\nkeep existing\n'
        result = package.replace_runtime_stage(original)
        repeated = package.replace_runtime_stage(result)
        for text in (result, repeated):
            self.assertTrue(text.endswith('keep existing\n'))
            self.assertEqual(text.count('mount --bind "$R/compat/sensor-param"'), 1)
            self.assertEqual(text.count(package.READY + ' = '), 1)
            self.assertNotIn('sha256sum', text)
            self.assertIn('cp -a "$R/system/etc/param/."', text)
            self.assertNotIn('remount', text)
        bash = shutil.which('bash') or 'C:/Program Files/Git/bin/bash.exe'
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'apply-runtime.sh'
            path.write_text(result, encoding='utf-8', newline='\n')
            run = subprocess.run([bash, '-n', str(path)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
        for name in package.SCRIPTS:
            path = ROOT / 'runtime/compat/sensors' / name
            self.assertNotIn(b'\r', path.read_bytes())
            run = subprocess.run([bash, '-n', str(path)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == '__main__':
    unittest.main()

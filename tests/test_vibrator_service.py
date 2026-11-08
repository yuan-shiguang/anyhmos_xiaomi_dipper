"""Check native SA3602 selection and execute its generated startup branches."""

import argparse
import ast
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
NAMES = {
    'sha',
    'validate_vibrator_inputs',
    'add_vibrator_service',
    'add_vibrator_startup',
}
HELPERS = ast.Module(
    body=[
        node
        for node in TREE.body
        if isinstance(node, ast.FunctionDef)
        and node.name in NAMES
        or isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name) and target.id == 'VIBRATOR_INPUTS'
            for target in node.targets
        )
    ],
    type_ignores=[],
)
FUNCTIONS = {'copy': copy, 'hashlib': hashlib, 're': re}
exec(compile(HELPERS, str(SOURCE), 'exec'), FUNCTIONS)
validate_inputs = FUNCTIONS['validate_vibrator_inputs']
add_service = FUNCTIONS['add_vibrator_service']
add_startup = FUNCTIONS['add_vibrator_startup']
ANCHOR = 'stage=scene\ntrace_stage\ndeadline=$(($(now)+600))\n'
DIRECTORIES = [
    'mkdir /data/service/el1/public/sensor',
    'chown sensor sensor /data/service/el1/public/sensor',
]


class VibratorServiceTests(unittest.TestCase):
    def setUp(self):
        self.desktop = {
            'services': [{'name': 'existing', 'uid': 'system'}],
            'jobs': [{'name': 'existing_job', 'cmds': ['keep existing log']}],
        }
        self.service = {
            'name': 'sensors',
            'path': ['/system/bin/sa_main', '/system/profile/sensors.json'],
            'uid': 'sensor',
            'gid': ['sensor', 'shell'],
            'secon': 'u:r:sensors:s0',
            'permission': ['original.permission.' + str(n) for n in range(9)],
            'permission_acls': ['ohos.permission.GET_SENSITIVE_PERMISSIONS'],
        }
        self.ability = {
            'name': 3602,
            'libpath': 'libmiscdevice_service.z.so',
            'run-on-create': True,
            'distributed': False,
            'dump_level': 1,
            'min_hdi_proxy_version': ['libvibrator_proxy_2.0.z.so'],
        }
        self.cfg = {
            'services': [self.service],
            'jobs': [
                {'name': 'boot', 'cmds': DIRECTORIES.copy()},
                {'name': 'init', 'cmds': ['write /sys/forbidden 1']},
            ],
        }
        self.profile = {
            'process': 'sensors',
            'systemability': [
                self.ability,
                {'name': 3601, 'libpath': 'libsensor_service.z.so'},
            ],
        }

    def test_option_defaults_to_disabled(self):
        parser = argparse.ArgumentParser()
        option = next(
            node
            for node in TREE.body
            if isinstance(node, ast.Expr)
            and isinstance(node.value, ast.Call)
            and node.value.args
            and isinstance(node.value.args[0], ast.Constant)
            and node.value.args[0].value == '--vibrator-service'
        )
        exec(
            compile(ast.Module(body=[option], type_ignores=[]), str(SOURCE), 'exec'),
            {'parser': parser},
        )
        self.assertFalse(parser.parse_args([]).vibrator_service)
        self.assertTrue(parser.parse_args(['--vibrator-service']).vibrator_service)

    def test_disabled_branches_do_not_extract_or_mutate_payload(self):
        branches = [
            node
            for node in TREE.body
            if isinstance(node, ast.If)
            and isinstance(node.test, ast.Attribute)
            and isinstance(node.test.value, ast.Name)
            and node.test.value.id == 'args'
            and node.test.attr == 'vibrator_service'
        ]
        self.assertEqual(len(branches), 3)
        scope = {'args': argparse.Namespace(vibrator_service=False)}
        exec(
            compile(ast.Module(body=branches, type_ignores=[]), str(SOURCE), 'exec'),
            scope,
        )
        self.assertEqual(set(scope), {'args', '__builtins__'})

    # 核对实际生成内容，只新增振动服务和原目录命令
    def test_selected_profile_and_native_identity_preserve_existing_configuration(self):
        before = copy.deepcopy(self.desktop)
        original = copy.deepcopy((self.cfg, self.profile))
        selected = add_service(self.desktop, self.cfg, self.profile)
        self.assertEqual(
            selected, {'process': 'sensors', 'systemability': [self.ability]}
        )
        self.assertEqual((self.cfg, self.profile), original)
        self.assertEqual(self.desktop['services'][:-1], before['services'])
        self.assertEqual(self.desktop['jobs'][:-2], before['jobs'])
        service = self.desktop['services'][-1]
        for key, value in self.service.items():
            if key != 'path':
                self.assertEqual(service[key], value)
        self.assertEqual(
            service['path'],
            ['/system/bin/sa_main', '/compat/etc/sensors-vibrator-only.json'],
        )
        self.assertEqual(service['disabled'], 1)
        self.assertEqual(service['once'], 1)
        self.assertEqual(service['start-mode'], 'condition')
        self.assertIs(service['saspawn'], False)
        self.assertEqual(
            self.desktop['jobs'][-2]['cmds'], DIRECTORIES + ['start sensors']
        )
        self.assertEqual(self.desktop['jobs'][-1]['cmds'], ['stop sensors'])
        for forbidden in (
            '3601',
            'hdf_devhost',
            'vibrator_host',
            '/sys/',
            '/dev/haptics',
            'chmod',
        ):
            self.assertNotIn(forbidden, json.dumps((selected, self.desktop)))

    def test_absent_wrong_or_duplicate_ability_is_rejected_atomically(self):
        profiles = [
            {'process': 'sensors', 'systemability': []},
            {'process': 'wrong', 'systemability': [self.ability]},
            {'process': 'sensors', 'systemability': [self.ability, self.ability]},
        ]
        for key, value in [
            ('libpath', 'fake.so'),
            ('run-on-create', False),
            ('min_hdi_proxy_version', []),
        ]:
            damaged = copy.deepcopy(self.profile)
            damaged['systemability'][0][key] = value
            profiles.append(damaged)
        for profile in profiles:
            desktop = copy.deepcopy(self.desktop)
            before = copy.deepcopy(desktop)
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                add_service(desktop, self.cfg, profile)
            self.assertEqual(desktop, before)

    def test_service_identity_permissions_or_directory_changes_are_rejected(self):
        for key, value in [
            ('uid', 'root'),
            ('gid', ['sensor']),
            ('secon', 'u:r:init:s0'),
            ('permission', []),
            ('permission_acls', []),
            ('path', ['/fake']),
        ]:
            cfg = copy.deepcopy(self.cfg)
            cfg['services'][0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                add_service(self.desktop, cfg, self.profile)
        for commands in ([], DIRECTORIES + ['write /sys/forbidden 1']):
            cfg = copy.deepcopy(self.cfg)
            cfg['jobs'][0]['cmds'] = commands
            with self.assertRaises(ValueError):
                add_service(self.desktop, cfg, self.profile)
        cfg = copy.deepcopy(self.cfg)
        cfg['services'].append(copy.deepcopy(self.service))
        with self.assertRaises(ValueError):
            add_service(self.desktop, cfg, self.profile)

    def test_existing_service_or_gate_conflicts_are_rejected_atomically(self):
        for key, item in [
            ('services', {'name': 'sensors'}),
            ('jobs', {'name': 'param:dipper-vibrator-service'}),
            ('jobs', {'name': 'param:dipper-vibrator-service-stop'}),
            ('jobs', {'name': 'other', 'condition': 'dipper.debug.start.vibrator=true'}),
            (
                'jobs',
                {'name': 'other', 'condition': 'dipper.debug.start.vibrator=false'},
            ),
        ]:
            desktop = copy.deepcopy(self.desktop)
            desktop[key].append(item)
            before = copy.deepcopy(desktop)
            with self.subTest(item=item), self.assertRaises(ValueError):
                add_service(desktop, self.cfg, self.profile)
            self.assertEqual(desktop, before)

    def test_input_set_hash_and_uid_gids_are_checked_independently(self):
        pins = FUNCTIONS['VIBRATOR_INPUTS']
        inputs = {path: path.encode() for path in pins}
        inputs['/system/etc/passwd'] = (
            b'sensor:x:6688:6688:::/bin/false\nshell:x:2000:2000:::/bin/false\n'
        )
        inputs['/system/etc/group'] = b'sensor:x:6688:\nshell:x:2000:\n'
        with patch.dict(
            pins,
            {path: hashlib.sha256(data).hexdigest() for path, data in inputs.items()},
        ):
            validate_inputs(inputs)
            for path in pins:
                damaged = dict(inputs)
                damaged[path] += b'changed'
                with self.subTest(path=path), self.assertRaisesRegex(
                    ValueError, 'hash mismatch'
                ):
                    validate_inputs(damaged)
            for filename in ('passwd', 'group'):
                path = '/system/etc/' + filename
                damaged = dict(inputs)
                damaged[path] = damaged[path].replace(b'6688', b'0')
                with patch.dict(
                    pins, {path: hashlib.sha256(damaged[path]).hexdigest()}
                ):
                    with self.assertRaisesRegex(ValueError, 'UID or GID mismatch'):
                        validate_inputs(damaged)
            with self.assertRaisesRegex(ValueError, 'input set'):
                validate_inputs({})

    def test_startup_anchor_is_unique(self):
        for source in ['', ANCHOR * 2, ANCHOR + 'VIBRATOR_START_REQUEST_FAILED=1\n']:
            with self.subTest(source=source), self.assertRaises(ValueError):
                add_startup(source)

    # 执行后台启动请求，保留失败日志及桌面继续
    def test_background_startup_preserves_real_staging_failures_without_polling(self):
        bash = shutil.which('bash') or 'C:/Program Files/Git/bin/bash.exe'
        generated = add_startup(ANCHOR + 'echo DESKTOP_CONTINUED=1\n')
        harness = """set -eu
B=bb
R=/unused
trace_stage() { :; }
now() { echo 0; }
bb() {
 case "$1" in
 mkdir) [ "$CASE" != mkdirfail ] ;;
 cp) [ "$CASE" != copyfail ] ;;
 chmod) echo "PROFILE_MODE=$2"; [ "$CASE" != chmodfail ] ;;
 chroot) echo "SERVICE_REQUEST=${*:4}"; [ "$CASE" != requestfail ] ;;
 *) return 99 ;;
 esac
}
"""
        for case in ['ready', 'mkdirfail', 'copyfail', 'chmodfail', 'requestfail']:
            with self.subTest(case=case):
                run = subprocess.run([bash, '-s'], input=harness + generated + 'wait\n',
                                     text=True, capture_output=True, timeout=5,
                                     env=dict(os.environ, CASE=case))
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertIn('DESKTOP_CONTINUED=1', run.stdout)
                self.assertEqual('SERVICE_REQUEST=' in run.stdout,
                                 case in ['ready', 'requestfail'])
                if case in ['mkdirfail', 'copyfail', 'chmodfail']:
                    self.assertIn('VIBRATOR_PROFILE_STAGE_FAILED=1', run.stdout)
                elif case == 'requestfail':
                    self.assertIn('VIBRATOR_START_REQUEST_FAILED=1', run.stdout)
                else:
                    self.assertIn('dipper.debug.start.vibrator true', run.stdout)
                    self.assertIn('PROFILE_MODE=0644', run.stdout)
        self.assertNotIn('sha256sum', generated)
        self.assertNotIn('native_sa_check', generated)
        self.assertNotIn('timeout', generated)

    @unittest.skipUnless(
        os.environ.get('HMOS_VIBRATOR_DONOR_INPUTS'),
        'private donor inputs not supplied',
    )
    def test_real_donor_contract(self):
        root = Path(os.environ['HMOS_VIBRATOR_DONOR_INPUTS'])
        inputs = {
            path: (root / path.lstrip('/')).read_bytes()
            for path in FUNCTIONS['VIBRATOR_INPUTS']
        }
        validate_inputs(inputs)
        cfg = json.loads(inputs['/system/etc/init/sensors_musl.cfg'])
        profile = json.loads(inputs['/system/profile/sensors.json'])
        original = copy.deepcopy(cfg['services'][0])
        selected = add_service(self.desktop, cfg, profile)
        self.assertEqual(selected['systemability'], [profile['systemability'][0]])
        for key in ('uid', 'gid', 'permission', 'permission_acls', 'secon'):
            self.assertEqual(self.desktop['services'][-1][key], original[key])
        self.assertEqual(len(original['permission']), 9)
        self.assertEqual(len(original['permission_acls']), 1)


if __name__ == '__main__':
    unittest.main()

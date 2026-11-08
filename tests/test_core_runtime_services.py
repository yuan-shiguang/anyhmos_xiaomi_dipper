"""Check opt-in native core service selection and generated startup failures."""

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
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
NAMES = {
    'sha',
    'validate_core_runtime_inputs',
    'add_core_runtime_services',
    'add_core_runtime_startup',
}
HELPERS = ast.Module(
    body=[
        node
        for node in TREE.body
        if isinstance(node, ast.FunctionDef)
        and node.name in NAMES
        or isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name) and target.id == 'CORE_RUNTIME_INPUTS'
            for target in node.targets
        )
    ],
    type_ignores=[],
)
FUNCTIONS = {'copy': copy, 'hashlib': hashlib, 're': re}
exec(compile(HELPERS, str(SOURCE), 'exec'), FUNCTIONS)
validate_inputs = FUNCTIONS['validate_core_runtime_inputs']
add_services = FUNCTIONS['add_core_runtime_services']
add_startup = FUNCTIONS['add_core_runtime_startup']
COPY = '$B cp "$R/data/local/tmp/foundation-app-core.json" "$R/system/profile/foundation.json"\n'
WAIT = 'stage=scene\ntrace_stage\ndeadline=$(($(now)+600))\n'
DIRECTORIES = [
    'mkdir /data/service/el1/public/ressched 0700 ressched ressched',
    'mkdir /data/service/el1/public/efficiency_manager 0700 ressched ressched',
    'mkdir /data/service/el1/public/device_standby 0700 ressched ressched',
    'mkdir /data/service/el1/public/appnap 0700 ressched ressched',
    'mkdir /data/service/el1/public/background_task_mgr 0711 ressched ressched',
    'mkdir /data/service/el1/public/WorkScheduler 0711 ressched ressched',
    'mkdir /data/service/el1/public/bundle_usage 0711 ressched ressched',
]


class CoreRuntimeServiceTests(unittest.TestCase):
    def setUp(self):
        self.desktop = {
            'services': [{'name': 'existing', 'uid': 'system', 'permission': ['keep']}],
            'jobs': [{'name': 'existing_job', 'cmds': ['keep existing log']}],
        }
        self.ces = {
            'name': 3299,
            'libpath': 'libcesfwk_services.z.so',
            'run-on-create': True,
        }
        self.foundation = {
            'process': 'foundation',
            'systemability': [{'name': 401, 'keep': True}, self.ces],
        }
        self.identity = {
            'name': 'foundation',
            'uid': 'foundation',
            'gid': ['system'],
            'permission': ['original'],
            'caps': ['KILL'],
        }
        self.resource = {
            'name': 'resource_schedule_service',
            'path': [
                '/system/bin/sa_main',
                '/system/profile/resource_schedule_service.json',
            ],
            'uid': 'ressched',
            'gid': ['ressched', 'system', 'battery_extra_host'],
            'caps': ['SYS_NICE'],
            'importance': -20,
            'sandbox': 0,
            'permission': ['native.permission.' + str(index) for index in range(67)],
            'permission_acls': [
                'native.permission.' + str(index) for index in range(7)
            ],
            'secon': 'u:r:resource_schedule_service:s0',
        }
        self.ans = {
            'name': 3203,
            'libpath': 'libans.z.so',
            'run-on-create': True,
            'depend': [3299],
            'depend_time_out': 60000,
            'extension': ['backup', 'restore'],
        }
        self.donor = {
            'foundation_cfg': {'services': [copy.deepcopy(self.identity)]},
            'foundation_profile': {
                'process': 'foundation',
                'systemability': [self.ans, {'name': 66568}],
            },
            'resource_cfg': {
                'services': [self.resource, {'name': 'pin_auth_host'}],
                'jobs': [
                    {
                        'name': 'post-fs-data',
                        'cmds': DIRECTORIES
                        + [
                            'syncexec /system/bin/chown -R ressched:ressched /data/keep',
                            'start resource_schedule_service',
                        ],
                    },
                    {
                        'name': 'pre-init',
                        'cmds': ['mount cgroup none /dev/cpuset cpuset'],
                    },
                    {'name': 'init', 'cmds': ['rm -rf /dev/frz', 'write /sys/keep 1']},
                ],
            },
            'resource_profile': {
                'process': 'resource_schedule_service',
                'systemability': [
                    {'name': 1910, 'libpath': 'unused-suspend.so'},
                    {
                        'name': 1903,
                        'libpath': 'libbgtaskmgr_service.z.so',
                        'run-on-create': True,
                    },
                    {
                        'name': 1901,
                        'libpath': 'libresschedsvc.z.so',
                        'run-on-create': True,
                    },
                    {'name': 1906, 'libpath': 'unused-socperf.so'},
                ],
            },
        }

    def test_option_is_off_unless_explicitly_requested(self):
        parser = argparse.ArgumentParser()
        declaration = next(
            node
            for node in TREE.body
            if isinstance(node, ast.Expr)
            and isinstance(node.value, ast.Call)
            and node.value.args
            and isinstance(node.value.args[0], ast.Constant)
            and node.value.args[0].value == '--core-runtime-services'
        )
        exec(
            compile(
                ast.Module(body=[declaration], type_ignores=[]), str(SOURCE), 'exec'
            ),
            {'parser': parser},
        )
        self.assertFalse(parser.parse_args([]).core_runtime_services)
        self.assertTrue(
            parser.parse_args(['--core-runtime-services']).core_runtime_services
        )

    # 检查生成配置保留原服务、权限及已有任务
    def test_generated_configs_preserve_native_contract_and_select_only_core(self):
        before = copy.deepcopy(self.desktop)
        donor = copy.deepcopy(self.donor)
        foundation = copy.deepcopy(self.foundation)
        updated, resource = add_services(
            self.desktop, self.foundation, self.identity, self.donor
        )
        generated = json.loads(
            json.dumps(
                {'desktop': self.desktop, 'foundation': updated, 'resource': resource}
            )
        )
        self.assertEqual(generated['desktop']['services'][:-1], before['services'])
        self.assertEqual(generated['desktop']['jobs'][:-1], before['jobs'])
        self.assertEqual(
            updated['systemability'], foundation['systemability'] + [self.ans]
        )
        self.assertEqual(self.foundation, foundation)
        self.assertEqual(self.donor, donor)
        selected = generated['desktop']['services'][-1]
        for key, value in self.resource.items():
            if key != 'path':
                self.assertEqual(selected[key], value)
        self.assertEqual(
            selected['path'],
            ['/system/bin/sa_main', '/compat/etc/resource-schedule-core.json'],
        )
        self.assertTrue(selected['disabled'])
        self.assertFalse(selected['saspawn'])
        self.assertEqual([sa['name'] for sa in resource['systemability']], [1901, 1903])
        self.assertEqual(
            self.desktop['jobs'][-1]['cmds'],
            DIRECTORIES + ['start resource_schedule_service'],
        )
        for forbidden in [
            'cpuset',
            '/dev/frz',
            '/sys/',
            'syncexec',
            'pin_auth_host',
            'XPERF',
        ]:
            self.assertNotIn(forbidden, json.dumps(self.desktop))

    def test_conflicting_native_service_or_gate_is_rejected_without_partial_changes(
        self,
    ):
        for kind, value in [
            ('services', {'name': 'resource_schedule_service'}),
            ('jobs', {'name': 'param:dipper-core-runtime-services'}),
        ]:
            desktop = copy.deepcopy(self.desktop)
            desktop[kind].append(value)
            before = copy.deepcopy(desktop)
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                add_services(desktop, self.foundation, self.identity, self.donor)
            self.assertEqual(desktop, before)

    def test_wrong_identity_permissions_library_or_directory_is_rejected(self):
        for change in [
            'uid',
            'gid',
            'permissions',
            'foundation',
            'library',
            'directory',
            'duplicate_ans',
        ]:
            donor, identity, foundation = copy.deepcopy(
                (self.donor, self.identity, self.foundation)
            )
            if change == 'uid':
                donor['resource_cfg']['services'][0]['uid'] = 'root'
            elif change == 'gid':
                donor['resource_cfg']['services'][0]['gid'] = ['root']
            elif change == 'permissions':
                donor['resource_cfg']['services'][0]['permission'].pop()
            elif change == 'foundation':
                identity['permission'] = []
            elif change == 'library':
                donor['resource_profile']['systemability'][1]['libpath'] = 'other.so'
            elif change == 'directory':
                donor['resource_cfg']['jobs'][0]['cmds'].remove(DIRECTORIES[0])
            else:
                foundation['systemability'].append(self.ans)
            before = copy.deepcopy(self.desktop)
            with self.subTest(change=change), self.assertRaises(ValueError):
                add_services(self.desktop, foundation, identity, donor)
            self.assertEqual(self.desktop, before)

    # 拒绝通知或公共事件启动契约缺失，且不留下部分配置
    def test_ans_and_common_event_autostart_contract_is_required(self):
        for change in [
            'ans_disabled',
            'ans_missing_autostart',
            'ans_non_boolean_autostart',
            'missing_ces',
            'duplicate_ces',
            'ces_library',
            'ces_disabled',
            'ces_missing_autostart',
            'ces_non_boolean_autostart',
        ]:
            desktop, foundation, identity, donor = copy.deepcopy(
                (self.desktop, self.foundation, self.identity, self.donor)
            )
            ans = donor['foundation_profile']['systemability'][0]
            ces = foundation['systemability'][1]
            if change == 'ans_disabled':
                ans['run-on-create'] = False
            elif change == 'ans_missing_autostart':
                del ans['run-on-create']
            elif change == 'ans_non_boolean_autostart':
                ans['run-on-create'] = 1
            elif change == 'missing_ces':
                foundation['systemability'].remove(ces)
            elif change == 'duplicate_ces':
                foundation['systemability'].append(copy.deepcopy(ces))
            elif change == 'ces_library':
                ces['libpath'] = 'other.so'
            elif change == 'ces_disabled':
                ces['run-on-create'] = False
            elif change == 'ces_missing_autostart':
                del ces['run-on-create']
            else:
                ces['run-on-create'] = 1
            before = copy.deepcopy((desktop, foundation, identity, donor))
            expected = 'ANS ability' if change.startswith('ans_') else 'common event'
            with self.subTest(change=change), self.assertRaisesRegex(
                ValueError, expected
            ):
                add_services(desktop, foundation, identity, donor)
            self.assertEqual((desktop, foundation, identity, donor), before)

    def test_hash_and_uid_guards_are_independent(self):
        inputs = {
            '/system/etc/passwd': b'ressched:x:1096:1096:::/bin/false\nfoundation:x:5523:5523:::/bin/false\n',
            '/system/etc/group': b'ressched:x:1096:\nsystem:x:1000:\nbattery_extra_host:x:5001:\nfoundation:x:5523:\n',
            '/system/lib64/libans.z.so': b'fixture bytes',
        }
        pins = {path: hashlib.sha256(data).hexdigest() for path, data in inputs.items()}
        with patch.dict(FUNCTIONS, {'CORE_RUNTIME_INPUTS': pins}):
            validate_inputs(inputs)
            for path in inputs:
                damaged = dict(inputs, **{path: inputs[path] + b'changed'})
                with self.subTest(path=path), self.assertRaisesRegex(
                    ValueError, 'hash mismatch'
                ):
                    validate_inputs(damaged)
            wrong_uid = dict(inputs)
            wrong_uid['/system/etc/passwd'] = inputs['/system/etc/passwd'].replace(
                b'1096:1096', b'0:0'
            )
            with patch.dict(
                pins,
                {
                    '/system/etc/passwd': hashlib.sha256(
                        wrong_uid['/system/etc/passwd']
                    ).hexdigest()
                },
            ):
                with self.assertRaisesRegex(ValueError, 'UID or GID mismatch'):
                    validate_inputs(wrong_uid)
            with self.assertRaisesRegex(ValueError, 'input set'):
                validate_inputs({})
        self.assertEqual(len(FUNCTIONS['CORE_RUNTIME_INPUTS']), 9)

    def test_missing_duplicate_or_already_modified_startup_is_rejected(self):
        for source in ['', WAIT * 2, COPY + WAIT + 'stage=core_runtime\n']:
            with self.subTest(source=source), self.assertRaises(ValueError):
                add_startup(source)

    # 执行原启动请求，保留复制失败并避免诊断轮询
    def test_generated_shell_requests_original_service_without_diagnostic_gates(self):
        bash = shutil.which('bash') or 'C:/Program Files/Git/bin/bash.exe'
        generated = add_startup(COPY + WAIT + 'echo DESKTOP_CONTINUED=1\n')
        harness = """set -eu
B=bb
R=/unused
trace_stage() { :; }
now() { echo 0; }
bb() {
 case "$1" in
 cp) echo PROFILE_COPIED=1; [ "$CASE" != copyfail ] ;;
 chroot) echo "SERVICE_REQUEST=${*:4}"; [ "$CASE" != requestfail ] ;;
 *) return 99 ;;
 esac
}
"""
        for case in ['ready', 'copyfail', 'requestfail']:
            with self.subTest(case=case):
                run = subprocess.run([bash, '-s'], input=harness + generated,
                                     text=True, capture_output=True, timeout=5,
                                     env=dict(os.environ, CASE=case))
                if case == 'copyfail':
                    self.assertNotEqual(run.returncode, 0)
                    self.assertNotIn('SERVICE_REQUEST=', run.stdout)
                else:
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertIn('dipper.debug.start.resourceschedule true', run.stdout)
                    self.assertIn('DESKTOP_CONTINUED=1', run.stdout)
                    self.assertEqual('CORE_RUNTIME_START_REQUEST_FAILED=1' in run.stdout,
                                     case == 'requestfail')
        self.assertNotIn('sha256sum', generated)
        self.assertNotIn('native_sa_check', generated)
        self.assertNotIn('PROFILE_FALLBACK', generated)

    @unittest.skipUnless(
        os.environ.get('HMOS_CORE_RUNTIME_DONOR_INPUTS'),
        'private donor inputs not supplied',
    )
    def test_real_donor_generates_the_minimal_native_configuration(self):
        root = Path(os.environ['HMOS_CORE_RUNTIME_DONOR_INPUTS'])
        inputs = {
            path: (root / path.lstrip('/')).read_bytes()
            for path in FUNCTIONS['CORE_RUNTIME_INPUTS']
        }
        validate_inputs(inputs)
        donor = {
            name: json.loads(inputs[path])
            for name, path in {
                'resource_cfg': '/system/etc/init/resource_schedule_service.cfg',
                'resource_profile': '/system/profile/resource_schedule_service.json',
                'foundation_cfg': '/system/etc/init/foundation.cfg',
                'foundation_profile': '/system/profile/foundation.json',
            }.items()
        }
        graphics = ROOT / 'runtime/compat/graphics'
        desktop = json.loads((graphics / 'init-desktop-services.cfg').read_text())
        foundation = json.loads((graphics / 'foundation-app-core.json').read_text())
        identity = next(
            s
            for s in json.loads(
                (ROOT / 'runtime/compat/hmos-init-debug.cfg').read_text()
            )['services']
            if s['name'] == 'foundation'
        )
        updated, resource = add_services(desktop, foundation, identity, donor)
        self.assertEqual(
            len(updated['systemability']), len(foundation['systemability']) + 1
        )
        self.assertEqual([sa['name'] for sa in resource['systemability']], [1901, 1903])
        self.assertEqual(updated['systemability'][-1]['depend'], [3299])
        self.assertEqual(len(desktop['services'][-1]['permission']), 67)
        self.assertEqual(len(desktop['services'][-1]['permission_acls']), 7)


if __name__ == '__main__':
    unittest.main()

"""Check the HuaweiID startup delta without running the image builder."""

import ast
import copy
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
NAMES = {'add_huawei_id_service', 'add_huawei_id_startup'}
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
HELPERS = ast.Module(
    body=[node for node in TREE.body if isinstance(node, ast.FunctionDef) and node.name in NAMES],
    type_ignores=[],
)
FUNCTIONS = {}
exec(compile(HELPERS, str(SOURCE), 'exec'), FUNCTIONS)
add_service = FUNCTIONS['add_huawei_id_service']
add_startup = FUNCTIONS['add_huawei_id_startup']
ANCHOR = 'stage=scene\ntrace_stage\ndeadline=$(($(now)+600))\n'


class HuaweiIdStartupTests(unittest.TestCase):
    def setUp(self):
        self.desktop = {
            'services': [{'name': 'existing_service', 'uid': 'system', 'permission': ['keep']}],
            'jobs': [{'name': 'existing_job', 'cmds': ['keep existing']}],
        }
        self.donor = {
            'services': [
                {
                    'name': 'huawei_id_svc',
                    'path': [
                        '/system/bin/sa_main',
                        '/system/profile/huawei_id_svc.json',
                    ],
                    'uid': 'huawei_id',
                    'gid': ['huawei_id', 'netsys_socket'],
                    'apl': 'system_basic',
                    'permission': ['original.permission'],
                    'secon': 'u:r:huaweiid_service:s0',
                    'jobs': {'on-start': 'services:huawei_id_svc'},
                }
            ],
            'jobs': [
                {
                    'name': 'services:huawei_id_svc',
                    'cmds': ['mkdir /data/service/el1/public/huaweiid/ 0710 huawei_id system'],
                }
            ],
        }

    def test_existing_entries_and_original_service_contract_are_preserved(self):
        before = copy.deepcopy(self.desktop)
        donor = copy.deepcopy(self.donor)
        add_service(self.desktop, self.donor)
        self.assertEqual(self.desktop['services'][:-1], before['services'])
        self.assertEqual(self.desktop['jobs'][:1], before['jobs'])
        self.assertEqual(self.desktop['jobs'][1], donor['jobs'][0])
        for key, value in donor['services'][0].items():
            self.assertEqual(self.desktop['services'][-1][key], value)
        self.assertEqual(self.donor, donor)
        self.assertEqual(self.desktop['jobs'][-1]['cmds'], ['start huawei_id_svc'])

    def test_duplicate_service_or_job_is_rejected(self):
        for kind, item in (
            ('services', {'name': 'huawei_id_svc'}),
            ('jobs', {'name': 'services:huawei_id_svc'}),
            ('jobs', {'name': 'param:dipper-huaweiid-service'}),
        ):
            with self.subTest(kind=kind, name=item['name']):
                desktop = copy.deepcopy(self.desktop)
                desktop[kind].append(item)
                with self.assertRaises(ValueError):
                    add_service(desktop, self.donor)

    def test_wrong_native_identity_is_rejected(self):
        self.donor['services'][0]['uid'] = 'root'
        with self.assertRaises(ValueError):
            add_service(self.desktop, self.donor)

    def test_startup_is_inserted_after_full_foundation_and_inputmethod(self):
        before = 'stage=netstack\n# foundation enabled\nstage=inputmethod\n' + ANCHOR
        output = add_startup(before)
        self.assertTrue(output.startswith(before.split(ANCHOR)[0]))
        self.assertTrue(output.endswith(ANCHOR))
        self.assertLess(output.index('stage=inputmethod'), output.index('stage=huaweiid'))

    def test_missing_duplicate_or_already_patched_anchor_is_rejected(self):
        for startup in ('stage=scene\n', ANCHOR * 2, 'stage=huaweiid\n' + ANCHOR):
            with self.subTest(startup=startup), self.assertRaises(ValueError):
                add_startup(startup)

    # 执行生成的等待分支，核对依赖缺失和启动失败均可继续
    def test_generated_shell_success_and_nonfatal_failure_paths(self):
        bash = shutil.which('bash') or r'C:\Program Files\Git\bin\bash.exe'
        block = add_startup(ANCHOR).split(ANCHOR)[0]
        script = (
            '''set -eu
B=bb
R=/unused
now() { cat "$CLOCK"; }
trace_stage() { echo "STAGE=$stage"; }
bb() {
 case "$1" in
 timeout) shift 2; "$@" ;;
 chroot)
  shift 2
  if [ "$1" = /compat/bin/native_sa_check ]; then
   [ "$2" != "$FAILED_SA" ]
  else
   echo GATE_REQUESTED=1
   return "$GATE_RESULT"
  fi
  ;;
 sleep) value=$(cat "$CLOCK"); echo "$((value+25))" > "$CLOCK" ;;
 *) return 99 ;;
 esac
}
'''
            + block
            + 'echo STARTUP_CONTINUED=1\n'
        )
        cases = (
            ('none', '0', 'HUAWEIID_SA65926_READY=1', True),
            ('200', '0', 'HUAWEIID_DEPENDENCY_NOT_READY=200', False),
            ('65926', '0', 'HUAWEIID_SA65926_NOT_READY=1', True),
            ('none', '7', 'HUAWEIID_START_REQUEST_FAILED=1', True),
        )
        for failed_sa, gate_result, expected, requested in cases:
            with self.subTest(failed_sa=failed_sa, gate_result=gate_result):
                with tempfile.TemporaryDirectory() as temporary:
                    clock = Path(temporary) / 'clock'
                    clock.write_text('0\n', encoding='utf-8')
                    env = dict(
                        os.environ,
                        CLOCK=clock.as_posix(),
                        FAILED_SA=failed_sa,
                        GATE_RESULT=gate_result,
                    )
                    result = subprocess.run(
                        [bash, '-s'],
                        input=script,
                        text=True,
                        capture_output=True,
                        env=env,
                        timeout=10,
                    )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(expected, result.stdout)
                self.assertIn('STARTUP_CONTINUED=1', result.stdout)
                self.assertEqual('GATE_REQUESTED=1' in result.stdout, requested)


if __name__ == '__main__':
    unittest.main()

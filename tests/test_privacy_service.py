"""Check privacy service identity and cold-start integration without target execution."""

import ast
import copy
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
HELPERS = ast.Module(
    body=[n for n in TREE.body if isinstance(n, ast.FunctionDef) and n.name == 'add_privacy_service'],
    type_ignores=[],
)
FUNCTIONS = {'copy': copy}
exec(compile(HELPERS, str(SOURCE), 'exec'), FUNCTIONS)
add_privacy = FUNCTIONS['add_privacy_service']


class PrivacyServiceTests(unittest.TestCase):
    def setUp(self):
        self.tracked = json.loads(
            (ROOT / 'runtime/compat/graphics/init-platform-v116.cfg').read_text(encoding='utf-8')
        )
        self.service = next(s for s in self.tracked['services'] if s['name'] == 'privacy_service')
        self.platform = {'services': [{'name': 'existing', 'permission': ['keep']}], 'jobs': []}
        self.native = {
            'import': ['/compat/etc/init-platform-v116.cfg'],
            'services': [{'name': 'accesstoken_service', 'permission': ['keep']}],
            'jobs': [{
                'name': 'param:dipper-debug-coredeps',
                'condition': 'dipper.debug.start.coredeps=true',
                'cmds': [
                    'mkdir /data/service/el1/public/access_token 0770 access_token access_token',
                    'start accesstoken_service',
                ],
            }],
        }

    def test_preserves_existing_services_and_initializes_on_core_start(self):
        before = copy.deepcopy(self.native)
        add_privacy(self.platform, self.native, self.tracked)
        self.assertEqual(self.platform['services'][0], {'name': 'existing', 'permission': ['keep']})
        self.assertEqual(self.platform['services'][1], self.service)
        self.assertEqual(self.native['services'], before['services'])
        self.assertEqual(self.native['import'], before['import'])
        job = self.native['jobs'][0]
        self.assertEqual(job['condition'], 'dipper.debug.start.coredeps=true')
        self.assertEqual(job['cmds'], before['jobs'][0]['cmds'] + ['start privacy_service'])

    def test_repacking_does_not_duplicate_service_or_start(self):
        add_privacy(self.platform, self.native, self.tracked)
        before = copy.deepcopy((self.platform, self.native))
        add_privacy(self.platform, self.native, self.tracked)
        self.assertEqual((self.platform, self.native), before)

    def test_refuses_conflicting_existing_declaration(self):
        self.platform['services'].append({'name': 'privacy_service', 'uid': 'root'})
        with self.assertRaises(ValueError):
            add_privacy(self.platform, self.native, self.tracked)

    def test_refuses_start_before_access_token(self):
        self.native['jobs'][0]['cmds'].insert(0, 'start privacy_service')
        with self.assertRaises(ValueError):
            add_privacy(self.platform, self.native, self.tracked)

    def test_native_identity_and_numeric_lifecycle(self):
        self.assertEqual(self.service['path'], ['/system/bin/sa_main', '/system/profile/privacy_service.json'])
        self.assertEqual(self.service['uid'], 'access_token')
        self.assertEqual(self.service['gid'], ['access_token'])
        self.assertEqual(self.service['secon'], 'u:r:privacy_service:s0')
        self.assertEqual(len(self.service['permission']), 18)
        self.assertEqual(len(self.service['permission_acls']), 6)
        self.assertIn('ohos.permission.PERMISSION_USED_STATS', self.service['permission'])
        self.assertIn('ohos.permission.MICROPHONE_CONTROL', self.service['permission_acls'])
        self.assertIs(type(self.service['disabled']), int)
        self.assertEqual(self.service['disabled'], 1)

    def test_tracked_init_starts_privacy_after_access_token(self):
        native = json.loads((ROOT / 'runtime/compat/hmos-init-debug.cfg').read_text(encoding='utf-8'))
        job = next(j for j in native['jobs'] if j['name'] == 'param:dipper-debug-coredeps')
        pos = job['cmds'].index('start accesstoken_service')
        self.assertEqual(job['cmds'][pos + 1], 'start privacy_service')

    def test_packaging_precedes_private_persist_derivation(self):
        calls = [n for n in ast.walk(TREE) if isinstance(n, ast.Call) and isinstance(n.func, ast.Name)]
        integrate = next(n.lineno for n in calls if n.func.id == 'add_privacy_service')
        derive = next(n.lineno for n in calls if n.func.id == 'prepare_private_persist')
        self.assertLess(integrate, derive)


if __name__ == '__main__':
    unittest.main()

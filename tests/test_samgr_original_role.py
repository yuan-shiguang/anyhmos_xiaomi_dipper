import ast
import copy
import hashlib
import json
import os
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import samgr_original_role as role
from private_persist_parameters import prepare_private_persist


SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
MEMBER = 'dipper-compat-v118/hmos-init-debug.cfg'
BOOTSTRAP = 'fb1545f6027ded68368b434dcb15e71b0003f2110a385d9ac2084df157e12407'


def encoded(value):
    return (json.dumps(value, indent=2) + '\n').encode()


# 执行主构建器的原写回阶段，核对角色与归档元数据
def builder_stage(config, donor, flags):
    inputs = next(node for node in TREE.body if isinstance(node, ast.Assign) and any(
        isinstance(target, ast.Name) and target.id == 'samgr_inputs'
        for target in node.targets
    ))
    restore = next(node for node in TREE.body if isinstance(node, ast.Assign) and
        isinstance(node.value, ast.Call) and isinstance(node.value.func, ast.Name) and
        node.value.func.id == 'restore_samgr_role')
    publish = next(node for node in TREE.body if isinstance(node, ast.Expr) and
        isinstance(node.value, ast.Call) and isinstance(node.value.func, ast.Name) and
        node.value.func.id == 'setfile' and len(node.value.args) == 2 and
        isinstance(node.value.args[1], ast.Name) and node.value.args[1].id == 'samgr_config')
    setter = next(node for node in TREE.body if isinstance(node, ast.FunctionDef) and
        node.name == 'setfile')
    private = next(node for node in ast.walk(TREE) if isinstance(node, ast.Call) and
        isinstance(node.func, ast.Name) and node.func.id == 'prepare_private_persist')
    if not inputs.lineno < restore.lineno < publish.lineno < private.lineno:
        raise AssertionError('Role restoration must precede private derivation')
    fields = [42, 0o100644, 0, 0, 1, 0, len(config), 0, 0, 0, 0, len(MEMBER) + 1, 0]
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        path = root / 'metadata/guard-inputs' / role.SAMGR_CONFIG.lstrip('/')
        path.parent.mkdir(parents=True)
        path.write_bytes(donor)
        namespace = {
            'ROOT': root, 'P': 'dipper-compat-v118/', 'args': flags,
            'new': {MEMBER: (fields, config)}, 'SAMGR_INPUTS': role.SAMGR_INPUTS,
            'restore_samgr_role': role.restore_samgr_role,
        }
        module = ast.Module(body=[setter, inputs, restore, publish], type_ignores=[])
        exec(compile(module, str(SOURCE), 'exec'), namespace)
        return namespace['new'][MEMBER], namespace['samgr_role_report']


# 直接读取原归档配置，避免模板掩盖实际缺口
def cpio_config(path):
    with path.open('rb') as stream:
        while True:
            header = stream.read(110)
            if header[:6] != b'070701':
                raise ValueError('Invalid input cpio')
            fields = [int(header[6 + index * 8:14 + index * 8], 16) for index in range(13)]
            name = stream.read(fields[11])[:-1].decode()
            stream.seek((-stream.tell()) % 4, 1)
            if name == 'TRAILER!!!':
                raise ValueError('Missing base init config')
            if name == MEMBER:
                return stream.read(fields[6])
            stream.seek(fields[6], 1)
            stream.seek((-stream.tell()) % 4, 1)


class SamgrOriginalRoleTests(unittest.TestCase):
    def setUp(self):
        self.identity = {
            'name': 'samgr', 'path': ['/system/bin/samgr'], 'uid': 'samgr',
            'gid': ['samgr', 'system', 'readproc', 'data_reserve'],
            'secon': 'u:r:samgr:s0',
        }
        self.donor = dict(self.identity, **{
            'start-mode': 'boot', 'critical': [1, 1, 60], 'importance': -20,
            'permission': ['permission.' + str(index) for index in range(22)],
            'permission_acls': ['permission.17', 'permission.21'],
        })
        self.config = {
            'jobs': [{'name': 'pre-init', 'cmds': [
                'load_persist_params', 'setparam persist.keep true',
            ]}],
            'services': [dict(self.identity, **{
                'start-mode': 'condition', 'disabled': 1, 'once': 1, 'sandbox': 0,
                'file': ['/dev/kmsg rd 0660 root system'],
            }), {'name': 'keep', 'uid': 'keep', 'permission': ['keep.permission']}],
            'import': ['/compat/etc/keep.cfg'],
        }
        self.donor_bytes = encoded({'services': [self.donor], 'jobs': [{'name': 'never-replay'}]})
        self.inputs = {role.SAMGR_CONFIG: self.donor_bytes}
        self.guard = patch.dict(role.SAMGR_INPUTS, {
            role.SAMGR_CONFIG: hashlib.sha256(self.donor_bytes).hexdigest(),
        }, clear=True)
        self.guard.start()
        self.addCleanup(self.guard.stop)

    def restore(self, config=None):
        data = encoded(self.config if config is None else config)
        return role.restore_samgr_role(data, self.inputs)

    def test_restores_only_original_role_fields(self):
        original = copy.deepcopy(self.config)
        output, report = self.restore()
        restored = json.loads(output)
        service = role.samgr_service(restored)
        for key in role.ROLE_FIELDS:
            self.assertEqual(service.pop(key), self.donor[key])
        self.assertEqual(restored, original)
        self.assertEqual(self.config, original)
        self.assertNotIn('critical', service)
        self.assertNotIn('importance', service)
        self.assertFalse(report['already_restored'])

    def test_exact_existing_role_keeps_original_bytes(self):
        output, _ = self.restore()
        compact = json.dumps(json.loads(output), separators=(',', ':')).encode()
        again, report = role.restore_samgr_role(compact, self.inputs)
        self.assertEqual(again, compact)
        self.assertTrue(report['already_restored'])
        self.assertEqual(report['input_sha256'], report['output_sha256'])

    def test_rejects_unknown_donor_inputs(self):
        for inputs in [{}, {role.SAMGR_CONFIG: self.donor_bytes + b' '},
            {**self.inputs, '/unexpected': b'unknown'}]:
            with self.subTest(inputs=list(inputs)):
                with self.assertRaises(ValueError):
                    role.restore_samgr_role(encoded(self.config), inputs)

    def test_rejects_partial_or_different_role(self):
        for fields in [
            {'permission': self.donor['permission']},
            {'permission_acls': self.donor['permission_acls']},
            {'permission': ['unknown'], 'permission_acls': []},
            {'permission': self.donor['permission'], 'permission_acls': ['unknown']},
        ]:
            config = copy.deepcopy(self.config)
            config['services'][0].update(fields)
            with self.subTest(fields=fields):
                with self.assertRaises(ValueError):
                    self.restore(config)

    def test_rejects_foreign_or_ambiguous_identity(self):
        for key, value in [('uid', 'root'), ('gid', ['root']), ('path', ['/other']),
            ('secon', 'u:r:other:s0'), ('apl', 'system_core')]:
            config = copy.deepcopy(self.config)
            config['services'][0][key] = value
            with self.subTest(field=key):
                with self.assertRaises(ValueError):
                    self.restore(config)
        for services in [[], [self.identity, self.identity], ['invalid']]:
            config = copy.deepcopy(self.config)
            config['services'] = services
            with self.assertRaises(ValueError):
                self.restore(config)

    def test_private_hash_chain_uses_repaired_main(self):
        output, _ = self.restore()
        template = (ROOT / 'runtime/compat/graphics/prepare-private-persist.sh').read_text(
            encoding='utf-8')
        private, helper, identities = prepare_private_persist(output, template, BOOTSTRAP)
        self.assertEqual(
            role.samgr_service(json.loads(private)), role.samgr_service(json.loads(output))
        )
        self.assertEqual(identities['@BASELINE_SHA@'], hashlib.sha256(output).hexdigest())
        self.assertEqual(identities['@CANDIDATE_SHA@'], hashlib.sha256(private).hexdigest())
        self.assertIn(identities['@BASELINE_SHA@'].encode(), helper)
        self.assertIn(identities['@CANDIDATE_SHA@'].encode(), helper)

    def test_both_main_and_private_buffer_limits(self):
        config = copy.deepcopy(self.config)
        config['padding'] = 'x'
        output, _ = self.restore(config)
        config['padding'] += 'x' * (32740 - len(output))
        output, _ = self.restore(config)
        self.assertEqual(len(output), 32740)
        template = '@BOOTSTRAP_SHA@ @BASELINE_SHA@ @CANDIDATE_SHA@'
        with self.assertRaises(ValueError):
            prepare_private_persist(output, template, BOOTSTRAP)
        config['padding'] += 'x' * 28
        with self.assertRaises(ValueError):
            self.restore(config)

    def test_builder_stage_is_unconditional_and_preserves_metadata(self):
        guards = [node for node in TREE.body if isinstance(node, ast.For) and
            isinstance(node.iter, ast.Name) and node.iter.id == 'SAMGR_INPUTS']
        self.assertEqual(len(guards), 1)
        for private in [False, True]:
            flags = SimpleNamespace(private_persist_parameters=private,
                core_runtime_services=False, cert_manager_service=False)
            (fields, output), report = builder_stage(encoded(self.config), self.donor_bytes, flags)
            self.assertEqual(fields[:6], [42, 0o100644, 0, 0, 1, 0])
            self.assertEqual(fields[6], len(output))
            self.assertEqual(
                role.samgr_service(json.loads(output))['permission'], self.donor['permission']
            )
            self.assertTrue(report['enabled'])


class SamgrActualInputTests(unittest.TestCase):
    def test_actual_6fd_builder_and_private_chain(self):
        base = os.environ.get('HMOS_SAMGR_BASE_CPIO')
        donor = os.environ.get('HMOS_SAMGR_DONOR_CONFIG')
        if not base or not donor:
            self.skipTest('Set actual base cpio and original donor config paths')
        config = cpio_config(Path(base))
        self.assertEqual(hashlib.sha256(config).hexdigest(),
            '369af218bd02a5e561e4559c07db3c039d9372f4ff0be5ac07bddc749b8258fc')
        donor_bytes = Path(donor).read_bytes()
        flags = SimpleNamespace(private_persist_parameters=True,
            core_runtime_services=False, cert_manager_service=False)
        (_, output), report = builder_stage(config, donor_bytes, flags)
        template = (ROOT / 'runtime/compat/graphics/prepare-private-persist.sh').read_text(
            encoding='utf-8')
        private, helper, identities = prepare_private_persist(output, template, BOOTSTRAP)
        expected = json.loads(donor_bytes)['services'][0]
        reconstructed = json.loads(output)
        service = role.samgr_service(reconstructed)
        for key in role.ROLE_FIELDS:
            self.assertEqual(service.pop(key), expected[key])
        self.assertEqual(reconstructed, json.loads(config))
        self.assertEqual(
            role.samgr_service(json.loads(private)), role.samgr_service(json.loads(output))
        )
        self.assertEqual(report['permission_count'], 22)
        self.assertEqual(report['permission_acl_count'], 2)
        self.assertIn(identities['@BASELINE_SHA@'].encode(), helper)
        self.assertIn(identities['@CANDIDATE_SHA@'].encode(), helper)


if __name__ == '__main__':
    unittest.main()

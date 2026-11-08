"""Verify the explicit IPC bundle and archive isolation without loading a module."""

import argparse
import ast
import copy
import hashlib
import importlib.util
import io
import json
import pathlib
import re
import struct
import subprocess
import sys
import unittest

from elftools.elf.elffile import ELFFile


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
HELPERS = {
    'inspect_linux_bootstrap', 'validate_bootstrap_binary',
    'validate_ipc_pair', 'replace_ipc_pair',
}
spec = importlib.util.spec_from_file_location(
    'bootstrap_fixtures', ROOT / 'tests/test_bootstrap_packaging.py'
)
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)


# 仅提取校验函数，不执行真实构包流程
def functions():
    nodes = [
        node for node in TREE.body
        if isinstance(node, ast.FunctionDef) and node.name in HELPERS
        or isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id.startswith('BOOTSTRAP_')
            for target in node.targets
        )
    ]
    namespace = {
        'hashlib': hashlib, 'io': io, 'json': json, 're': re, 'ELFFile': ELFFile,
    }
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), namespace)
    return namespace


# 使用独立夹具核对拒绝条件与归档边界
class IpcPairPackagingTests(unittest.TestCase):
    def setUp(self):
        self.ns = functions()
        self.bootstrap = fixtures.linux_elf()
        self.contract = self.bootstrap + b'private contract fixture'
        module = bytearray(self.bootstrap)
        struct.pack_into('<H', module, 16, 1)
        self.module = bytes(module)
        self.previous = b'previous IPC module fixture'
        self.expected = self.ns['BOOTSTRAP_IPC_PAIR']
        for name, blob in (
            ('bootstrap', self.bootstrap), ('module', self.module),
            ('contract', self.contract), ('module_input', self.previous),
        ):
            self.expected[name + '_sha256'] = hashlib.sha256(blob).hexdigest()
            self.expected[name + '_bytes'] = len(blob)
        self.ns['BOOTSTRAP_CANDIDATES'] = {
            self.expected['bootstrap_sha256']: {'variant': 'test-fixture'},
        }
        self.member = self.expected['module_member']
        self.manifest_name = 'dipper-compat-v118/MANIFEST.json'
        self.fields = [31, 0o100600, 0, 0, 1, 1234, len(self.previous), 0, 0, 0, 0, 30, 0]
        manifest = {'hmos-ipc.ko': {
            'bytes': len(self.previous), 'mode': '0o100600',
            'sha256': self.expected['module_input_sha256'],
        }}
        self.entries = {
            self.ns['BOOTSTRAP_MEMBER']: ([0] * 13, self.bootstrap),
            self.member: (self.fields.copy(), self.previous),
            self.manifest_name: ([0] * 13, json.dumps(manifest).encode()),
            'unchanged-sync-library': ([91] * 13, b'TF_SYNC frozen baseline'),
        }

    def validate(self, bootstrap=None, module=None, contract=None, **hashes):
        return self.ns['validate_ipc_pair'](
            self.bootstrap if bootstrap is None else bootstrap,
            self.module if module is None else module,
            self.contract if contract is None else contract,
            hashes.get('module_sha256', self.expected['module_sha256']),
            hashes.get('contract_sha256', self.expected['contract_sha256']),
        )

    def test_default_flag_is_off_and_paths_are_unset(self):
        nodes = [
            node for node in TREE.body
            if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == 'parser'
                for target in node.targets
            )
            or isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
            and isinstance(node.value.func, ast.Attribute)
            and isinstance(node.value.func.value, ast.Name)
            and node.value.func.value.id == 'parser'
            and node.value.func.attr == 'add_argument'
        ]
        namespace = {'argparse': argparse, 'pathlib': pathlib, 'REPO': ROOT, '__doc__': 'test'}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), namespace)
        args = namespace['parser'].parse_args(
            ['--donor', 'unused', '--original', 'unused', '--tools', 'unused',
             '--audio-build-dir', 'unused']
        )
        self.assertFalse(args.ipc_oneway_pid)
        for name in ('ipc_module', 'ipc_module_sha256', 'ipc_contract', 'ipc_contract_sha256'):
            self.assertIsNone(getattr(args, name))

    def test_cli_rejects_missing_flag_or_incomplete_bundle_before_file_access(self):
        for options, error in (
            (['--ipc-module', 'missing'], b'require --ipc-oneway-pid'),
            (['--ipc-contract-sha256', '0' * 64], b'require --ipc-oneway-pid'),
            (['--ipc-oneway-pid'], b'requires a bootstrap, module, contract'),
        ):
            result = subprocess.run(
                [sys.executable, str(SOURCE), '--donor', 'missing', '--original', 'missing',
                 '--tools', 'missing', '--audio-build-dir', 'missing', *options], capture_output=True,
            )
            with self.subTest(options=options):
                self.assertEqual(result.returncode, 2)
                self.assertIn(error, result.stderr)
                self.assertNotIn(b'FileNotFoundError', result.stderr)

    def test_new_bootstrap_requires_explicit_pair_option(self):
        with self.assertRaises(ValueError):
            self.ns['validate_bootstrap_binary'](
                self.bootstrap, self.expected['bootstrap_sha256']
            )
        self.ns['validate_bootstrap_binary'](
            self.bootstrap, self.expected['bootstrap_sha256'], ipc_pair=True
        )

    def test_old_bootstrap_and_new_module_are_rejected(self):
        with self.assertRaises(ValueError):
            self.validate(bootstrap=b'previous bootstrap')

    def test_new_bootstrap_and_old_module_are_rejected(self):
        with self.assertRaises(ValueError):
            self.validate(module=self.previous)

    def test_declared_hash_content_size_and_client_identity_are_fixed(self):
        cases = [
            {'module_sha256': '0' * 64}, {'contract_sha256': '0' * 64},
            {'module': self.module + b'x'}, {'contract': self.contract[:-1]},
            {'contract': self.contract[:-1] + b'x'},
        ]
        for case in cases:
            with self.subTest(case=list(case)), self.assertRaises(ValueError):
                self.validate(**case)

    def test_pair_reports_static_private_contract_without_runtime_claim(self):
        report = self.validate()
        self.assertTrue(report['enabled'])
        self.assertFalse(report['runtime_validated'])
        self.assertFalse(report['system_context_tested'])
        self.assertFalse(report['contract_elf']['dynamic_loader_required'])

    def test_replacement_preserves_module_fields_and_unrelated_sync(self):
        before = copy.deepcopy(self.entries)
        report = self.ns['replace_ipc_pair'](self.entries, self.module, self.contract)
        fields, blob = self.entries[self.member]
        self.assertEqual(blob, self.module)
        self.assertEqual(fields[:6] + fields[7:], self.fields[:6] + self.fields[7:])
        self.assertEqual(fields[6], len(self.module))
        self.assertEqual(before[self.member][0], self.fields)
        self.assertEqual(self.entries['unchanged-sync-library'], before['unchanged-sync-library'])
        self.assertTrue(report['builtin_contract_before_handoff_only'])
        self.assertFalse(report['contract_automatically_executed'])
        self.assertEqual(report['expected_retained_mode'], 1)

    def test_client_and_alias_have_exact_paths_modes_and_root_ownership(self):
        self.ns['replace_ipc_pair'](self.entries, self.module, self.contract)
        program, alias = self.expected['contract_member'], self.expected['contract_alias']
        self.assertEqual(self.entries[program][1], self.contract)
        self.assertEqual(self.entries[alias][1], ('/' + program).encode())
        for path, mode in ((program, 0o100755), (alias, 0o120777)):
            self.assertEqual(self.entries[path][0][1:5], [mode, 0, 0, 1])
            self.assertEqual(self.entries[path][0][6], len(self.entries[path][1]))

    def test_missing_member_or_colliding_private_path_rejected_without_mutation(self):
        for missing in (self.ns['BOOTSTRAP_MEMBER'], self.member, self.manifest_name):
            entries = copy.deepcopy(self.entries)
            del entries[missing]
            before = copy.deepcopy(entries)
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                self.ns['replace_ipc_pair'](entries, self.module, self.contract)
            self.assertEqual(entries, before)
        for key in ('contract_member', 'contract_alias'):
            entries = copy.deepcopy(self.entries)
            entries[self.expected[key]] = ([0] * 13, b'collision')
            before = copy.deepcopy(entries)
            with self.subTest(collision=key), self.assertRaises(ValueError):
                self.ns['replace_ipc_pair'](entries, self.module, self.contract)
            self.assertEqual(entries, before)

    def test_old_module_metadata_and_manifest_identity_are_required(self):
        for index, value in ((1, 0o100644), (2, 1000), (3, 1000), (4, 2), (6, 1)):
            entries = copy.deepcopy(self.entries)
            entries[self.member][0][index] = value
            before = copy.deepcopy(entries)
            with self.subTest(field=index), self.assertRaises(ValueError):
                self.ns['replace_ipc_pair'](entries, self.module, self.contract)
            self.assertEqual(entries, before)
        entries = copy.deepcopy(self.entries)
        entries[self.manifest_name] = ([0] * 13, b'{}')
        with self.assertRaises(ValueError):
            self.ns['replace_ipc_pair'](entries, self.module, self.contract)


if __name__ == '__main__':
    unittest.main()

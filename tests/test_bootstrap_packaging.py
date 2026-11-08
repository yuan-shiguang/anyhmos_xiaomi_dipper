"""Check explicit Linux collector selection and archive metadata boundaries."""

import argparse
import ast
import copy
import hashlib
import io
import json
import pathlib
from pathlib import Path
import re
import struct
import subprocess
import sys
import unittest

from elftools.elf.elffile import ELFFile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
NAMES = {'inspect_linux_bootstrap', 'validate_bootstrap_binary', 'replace_bootstrap'}
HELPERS = ast.Module(
    body=[
        node for node in TREE.body
        if isinstance(node, ast.FunctionDef) and node.name in NAMES
        or isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id.startswith('BOOTSTRAP_')
            for target in node.targets
        )
    ],
    type_ignores=[],
)


def functions():
    namespace = {
        'hashlib': hashlib, 'io': io, 'json': json, 're': re, 'ELFFile': ELFFile,
    }
    exec(compile(HELPERS, str(SOURCE), 'exec'), namespace)
    return namespace


def linux_elf():
    blob = bytearray(640)
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    struct.pack_into(
        '<16sHHIQQQIHHHHHH', blob, 0,
        ident, 2, 183, 1, 0x400100, 64, 576, 0, 64, 56, 3, 64, 1, 0,
    )
    struct.pack_into('<IIQQQQQQ', blob, 64, 1, 5, 0, 0x400000, 0x400000, 640, 640, 4096)
    struct.pack_into('<IIQQQQQQ', blob, 120, 0x6474E551, 6, 0, 0, 0, 0, 0, 16)
    struct.pack_into('<IIQQQQQQ', blob, 176, 4, 4, 512, 0x400200, 0x400200, 32, 32, 4)
    blob[512:544] = struct.pack('<III4sIIII', 4, 16, 1, b'GNU\0', 0, 3, 7, 0)
    return bytes(blob)


class BootstrapPackagingTests(unittest.TestCase):
    def setUp(self):
        self.ns = functions()
        self.inspect = self.ns['inspect_linux_bootstrap']
        self.validate = self.ns['validate_bootstrap_binary']
        self.replace = self.ns['replace_bootstrap']
        self.blob = linux_elf()
        self.digest = hashlib.sha256(self.blob).hexdigest()
        self.previous = b'previous collector fixture'
        self.previous_sha = hashlib.sha256(self.previous).hexdigest()
        self.ns.update({
            'BOOTSTRAP_INPUT_BYTES': len(self.previous),
            'BOOTSTRAP_INPUT_SHA256': self.previous_sha,
            'BOOTSTRAP_CANDIDATES': {self.digest: {'variant': 'test-fixture'}},
        })
        self.member = self.ns['BOOTSTRAP_MEMBER']
        self.manifest_name = self.member.rsplit('/', 1)[0] + '/MANIFEST.json'
        self.fields = [7, 0o100755, 0, 0, 1, 123, len(self.previous), 0, 0, 0, 0, 29, 0]
        manifest = {'hmos-probe': {
            'bytes': len(self.previous), 'mode': '0o100755', 'sha256': self.previous_sha,
        }}
        self.entries = {
            self.member: (self.fields.copy(), self.previous),
            self.manifest_name: ([0] * 13, json.dumps(manifest).encode()),
            'unrelated': ([19] * 13, b'keep'),
        }

    def reject_elf(self, offset, fmt, value):
        blob = bytearray(self.blob)
        struct.pack_into(fmt, blob, offset, value)
        with self.assertRaises(ValueError):
            self.inspect(bytes(blob))

    def test_valid_linux_elf(self):
        result = self.inspect(self.blob)
        self.assertEqual(result['gnu_linux_abi_minimum_kernel'], [[3, 7, 0]])
        self.assertEqual(result['gnu_stack_flags'], [6])
        self.assertFalse(result['dynamic_loader_required'])

    def test_dynamic_executable_and_wrong_architecture_rejected(self):
        self.reject_elf(16, '<H', 3)
        self.reject_elf(18, '<H', 62)
        self.reject_elf(176, '<I', 2)
        self.reject_elf(176, '<I', 3)

    def test_entry_must_be_aligned_executable_file_content(self):
        self.reject_elf(24, '<Q', 0x600000)
        self.reject_elf(24, '<Q', 0x400101)
        self.reject_elf(68, '<I', 4)
        blob = bytearray(self.blob)
        struct.pack_into('<Q', blob, 104, 1024)
        struct.pack_into('<Q', blob, 24, 0x400300)
        with self.assertRaises(ValueError):
            self.inspect(bytes(blob))

    def test_writable_code_or_executable_missing_stack_rejected(self):
        self.reject_elf(68, '<I', 7)
        self.reject_elf(124, '<I', 7)
        self.reject_elf(120, '<I', 0)

    def test_header_segment_and_load_bounds_rejected(self):
        self.reject_elf(32, '<Q', 600)
        self.reject_elf(56, '<H', 129)
        self.reject_elf(60, '<H', 0)
        self.reject_elf(96, '<Q', 641)
        self.reject_elf(104, '<Q', 639)
        self.reject_elf(112, '<Q', 3)

    def test_elf_note_is_not_sufficient_to_identify_collector(self):
        self.ns['BOOTSTRAP_CANDIDATES'] = {}
        with self.assertRaisesRegex(ValueError, 'role and Linux runtime'):
            self.validate(self.blob, self.digest)

    def test_linux_note_required_even_for_hash_selected_input(self):
        blob = bytearray(self.blob)
        struct.pack_into('<I', blob, 528, 1)
        with self.assertRaisesRegex(ValueError, 'GNU Linux ABI note'):
            self.inspect(bytes(blob))

    def test_hash_format_and_contents_rejected(self):
        with self.assertRaises(ValueError):
            self.validate(self.blob, self.digest.upper())
        with self.assertRaises(ValueError):
            self.validate(self.blob, '0' * 64)

    def test_only_contents_and_size_change_and_original_is_not_aliased(self):
        original = copy.deepcopy(self.entries)
        previous_fields = self.entries[self.member][0]
        report = self.replace(self.entries, self.blob, self.digest)
        self.assertEqual(set(self.entries), set(original))
        self.assertEqual(self.entries['unrelated'], original['unrelated'])
        self.assertEqual(self.entries[self.manifest_name], original[self.manifest_name])
        expected = original[self.member][0].copy()
        expected[6] = len(self.blob)
        self.assertEqual(self.entries[self.member], (expected, self.blob))
        self.assertEqual(previous_fields, original[self.member][0])
        self.assertIsNot(previous_fields, self.entries[self.member][0])
        self.assertEqual(report['input_sha256'], self.previous_sha)
        self.assertEqual(report['output_sha256'], self.digest)
        self.assertFalse(report['runtime_validated'])

    def test_wrong_member_identity_fails_before_mutation(self):
        for index, value in [(1, 0o100644), (2, 1000), (3, 1000), (4, 2), (6, 1)]:
            entries = copy.deepcopy(self.entries)
            entries[self.member][0][index] = value
            before = copy.deepcopy(entries)
            with self.subTest(field=index), self.assertRaises(ValueError):
                self.replace(entries, self.blob, self.digest)
            self.assertEqual(entries, before)

    def test_missing_or_wrong_manifest_and_member_fail_closed(self):
        for key in (self.member, self.manifest_name):
            entries = copy.deepcopy(self.entries)
            del entries[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                self.replace(entries, self.blob, self.digest)
        entries = copy.deepcopy(self.entries)
        entries[self.manifest_name] = ([0] * 13, b'{}')
        before = copy.deepcopy(entries)
        with self.assertRaises(ValueError):
            self.replace(entries, self.blob, self.digest)
        self.assertEqual(entries, before)

    def test_disabled_branch_does_not_touch_archive(self):
        nodes = [
            node for node in TREE.body
            if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == 'bootstrap_report'
                for target in node.targets
            )
            or isinstance(node, ast.If) and ast.unparse(node.test) == 'bootstrap_blob is not None'
        ]
        namespace = {'bootstrap_blob': None, 'new': self.entries}
        before = copy.deepcopy(self.entries)
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), namespace)
        self.assertEqual(self.entries, before)
        self.assertEqual(namespace['bootstrap_report'], {'enabled': False})

    def test_cli_pair_required_before_inputs_are_opened(self):
        for option in [
            ['--bootstrap', 'missing'], ['--bootstrap-sha256', '0' * 64],
        ]:
            result = subprocess.run(
                [sys.executable, str(SOURCE), '--donor', 'missing', '--original', 'missing',
                 '--tools', 'missing', '--audio-build-dir', 'missing', *option], capture_output=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn(b'must be supplied together', result.stderr)

    def test_cli_default_does_not_select_bootstrap(self):
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
        self.assertIsNone(args.bootstrap)
        self.assertIsNone(args.bootstrap_sha256)


if __name__ == '__main__':
    unittest.main()

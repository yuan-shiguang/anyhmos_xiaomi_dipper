"""Check the gated single-callsite restoration and exact reversal in memory."""

import argparse
import ast
import copy
import hashlib
import json
import os
from pathlib import Path
import pathlib
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
RECIPE = ROOT / 'patches/display/render-commit-async.json'


# 直接提取实际校验函数，避免执行构包入口
def functions():
    selected = [
        node for node in TREE.body
        if isinstance(node, ast.FunctionDef) and node.name == 'patch_render_commit_async'
        or isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in ('RENDER_COMMIT_ASYNC', 'BOOTSTRAP_IPC_PAIR')
            for target in node.targets
        )
    ]
    namespace = {'hashlib': hashlib}
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(SOURCE), 'exec'), namespace)
    return namespace


class RenderCommitAsyncTests(unittest.TestCase):
    def setUp(self):
        self.ns = functions()
        self.patch = self.ns['patch_render_commit_async']
        self.recipe = json.loads(RECIPE.read_text(encoding='utf-8'))
        self.pair = {'enabled': True}
        self.pair.update({
            name + '_sha256': self.ns['BOOTSTRAP_IPC_PAIR'][name + '_sha256']
            for name in ('bootstrap', 'module', 'contract')
        })

    def test_recipe_is_the_fixed_single_instruction(self):
        self.assertEqual(self.recipe, self.ns['RENDER_COMMIT_ASYNC'])
        self.assertEqual(self.recipe['changes'], [
            {'offset': 9245920, 'before': '01008052', 'after': '21008052'},
        ])

    def test_option_is_default_off(self):
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
        args = namespace['parser'].parse_args([
            '--donor', 'unused', '--original', 'unused', '--tools', 'unused',
            '--audio-build-dir', 'unused',
        ])
        self.assertFalse(args.render_commit_async)

    def test_cli_dependency_rejected_before_any_input_is_read(self):
        result = subprocess.run(
            [sys.executable, str(SOURCE), '--donor', 'missing', '--original', 'missing',
             '--tools', 'missing', '--audio-build-dir', 'missing',
             '--render-commit-async'], capture_output=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn(b'--render-commit-async requires --ipc-oneway-pid', result.stderr)
        self.assertNotIn(b'FileNotFoundError', result.stderr)

    def test_unverified_disabled_and_mixed_pairs_are_rejected(self):
        pairs = [None, {}, {**self.pair, 'enabled': False}]
        pairs.extend({**self.pair, name + '_sha256': '0' * 64}
                     for name in ('bootstrap', 'module', 'contract'))
        for pair in pairs:
            with self.subTest(pair=pair), self.assertRaises(ValueError):
                self.patch(b'', self.recipe, pair)

    def test_changed_recipe_target_hash_offset_or_context_is_rejected(self):
        for name, value in (
            ('path', '/system/lib64/libipc_single.z.so'),
            ('input_sha256', '0' * 64), ('output_sha256', '0' * 64), ('bytes', 4),
        ):
            recipe = copy.deepcopy(self.recipe)
            recipe[name] = value
            with self.subTest(field=name), self.assertRaises(ValueError):
                self.patch(b'', recipe, self.pair)
        for key in ('changes', 'guard'):
            recipe = copy.deepcopy(self.recipe)
            recipe[key] = [] if key == 'changes' else {}
            with self.subTest(field=key), self.assertRaises(ValueError):
                self.patch(b'', recipe, self.pair)

    def test_wrong_blob_identity_is_rejected(self):
        with self.assertRaises(ValueError):
            self.patch(b'not the reviewed library', self.recipe, self.pair)

    def test_disabled_branch_does_not_read_recipe_or_patch_library(self):
        nodes = [
            node for node in TREE.body
            if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == 'render_async_report'
                for target in node.targets
            )
            or isinstance(node, ast.If) and ast.unparse(node.test) == 'args.render_commit_async'
        ]
        namespace = {'args': argparse.Namespace(render_commit_async=False)}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), namespace)
        self.assertEqual(namespace['render_async_report'], {'enabled': False})

    @unittest.skipUnless(os.environ.get('HMOS_RENDER_COMMIT_SYNC_LIBRARY'), 'Reviewed SYNC library is required')
    def test_actual_patched_library_one_byte_restoration_and_exact_rollback(self):
        # 核对完整真实库及保留的Vulkan补丁
        blob = Path(os.environ['HMOS_RENDER_COMMIT_SYNC_LIBRARY']).read_bytes()
        result, report = self.patch(blob, self.recipe, self.pair)
        self.assertEqual(hashlib.sha256(result).hexdigest(), self.recipe['output_sha256'])
        self.assertEqual([i for i, (a, b) in enumerate(zip(blob, result)) if a != b], [9245920])
        self.assertEqual(result[9447112:9447116].hex(), '09008452')
        self.assertEqual(result[9451828:9451832].hex(), 'e9028052')
        restored = result[:9245920] + bytes.fromhex('01008052') + result[9245924:]
        self.assertEqual(restored, blob)
        self.assertTrue(report['commit_async_restored'])
        self.assertFalse(report['runtime_validated'])
        self.assertEqual(report['changed_byte_offsets'], [9245920])
        for invalid in (result, blob[:-1], bytes([blob[0] ^ 1]) + blob[1:]):
            with self.assertRaises(ValueError):
                self.patch(invalid, self.recipe, self.pair)


if __name__ == '__main__':
    unittest.main()

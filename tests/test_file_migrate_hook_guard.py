"""Check optional hook selection, pinned identities and generated RAM staging."""

import argparse
import ast
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
NAMES = {'sha', 'validate_file_migrate_hook', 'add_file_migrate_hook_guard'}
HELPERS = ast.Module(
    body=[
        node for node in TREE.body
        if isinstance(node, ast.FunctionDef) and node.name in NAMES
        or isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == 'FILE_MIGRATE_HOOK'
            for target in node.targets
        )
    ],
    type_ignores=[],
)
FUNCTIONS = {'hashlib': hashlib}
exec(compile(HELPERS, str(SOURCE), 'exec'), FUNCTIONS)
validate = FUNCTIONS['validate_file_migrate_hook']
stage = FUNCTIONS['add_file_migrate_hook_guard']
EXPECTED = FUNCTIONS['FILE_MIGRATE_HOOK']
HEAD = '#!/busybox sh\nset -eu\nB=/busybox\nR=/hmos-root\n'
RECIPE = ROOT / 'patches/init/file-migrate-hook-guard.json'


class FileMigrateSelectionTests(unittest.TestCase):
    def test_option_defaults_to_disabled(self):
        parser = argparse.ArgumentParser()
        option = next(
            node for node in TREE.body
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
            and node.value.args and isinstance(node.value.args[0], ast.Constant)
            and node.value.args[0].value == '--file-migrate-hook-guard'
        )
        exec(compile(ast.Module(body=[option], type_ignores=[]), str(SOURCE), 'exec'),
             {'parser': parser})
        self.assertFalse(parser.parse_args([]).file_migrate_hook_guard)
        self.assertTrue(parser.parse_args(['--file-migrate-hook-guard']).file_migrate_hook_guard)

    def test_disabled_branches_do_not_load_stage_or_add_payload(self):
        branches = [
            node for node in TREE.body
            if isinstance(node, ast.If) and isinstance(node.test, ast.Attribute)
            and isinstance(node.test.value, ast.Name) and node.test.value.id == 'args'
            and node.test.attr == 'file_migrate_hook_guard'
        ]
        self.assertEqual(len(branches), 4)
        scope = {'args': argparse.Namespace(file_migrate_hook_guard=False)}
        exec(compile(ast.Module(body=branches, type_ignores=[]), str(SOURCE), 'exec'), scope)
        self.assertEqual(set(scope), {'args', '__builtins__'})

    def test_staging_rejects_ambiguous_or_repeated_anchor(self):
        for before in ('', HEAD + HEAD, HEAD + EXPECTED['path'], stage(HEAD)):
            with self.subTest(before=before), self.assertRaises(ValueError):
                stage(before)


class FileMigrateInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        original = os.environ.get('HMOS_FILE_MIGRATE_ORIGINAL')
        guarded = os.environ.get('HMOS_FILE_MIGRATE_GUARDED')
        index = os.environ.get('HMOS_FILE_MIGRATE_INDEX')
        if not all((original, guarded, index)):
            raise unittest.SkipTest('Pinned donor inputs are required')
        cls.original = Path(original).read_bytes()
        cls.guarded = Path(guarded).read_bytes()
        cls.records = json.loads(Path(index).read_text(encoding='utf-8'))
        cls.recipe = json.loads(RECIPE.read_text(encoding='utf-8'))

    def test_actual_donor_identity_and_guard_output(self):
        report = validate(self.recipe, self.records, self.original)
        self.assertTrue(report['enabled'])
        self.assertFalse(report['runtime_validated'])
        self.assertEqual(report['uid'], 0)
        self.assertEqual(report['gid'], 0)
        self.assertEqual(report['mode'], '0644')
        self.assertEqual(report['path'], EXPECTED['path'])
        self.assertEqual(len(self.guarded), EXPECTED['bytes'])
        self.assertEqual(hashlib.sha256(self.guarded).hexdigest(), EXPECTED['output_sha256'])

    def test_wrong_path_hash_size_and_metadata_are_rejected(self):
        for key, value in [('path', '/system/lib64/wrong.so'),
                           ('input_sha256', '0' * 64), ('output_sha256', '0' * 64)]:
            recipe = copy.deepcopy(self.recipe)
            recipe[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate(recipe, self.records, self.original)
        for content in (self.original[:-1], b'X' + self.original[1:]):
            with self.assertRaises(ValueError):
                validate(self.recipe, self.records, content)
        record = next(row for row in self.records if row['path'] == EXPECTED['path'])
        for key, value in [('uid', 2000), ('gid', 2000), ('mode', '0755'), ('type', 'SYMLINK')]:
            damaged = dict(record)
            damaged[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate(self.recipe, [damaged], self.original)
        for records in ([], [record, record]):
            with self.assertRaises(ValueError):
                validate(self.recipe, records, self.original)

    # 执行生成脚本，异常身份或内容不得覆盖原加载路径
    def test_generated_staging_success_and_rejections(self):
        bash = Path(os.environ.get('HMOS_TEST_BASH', r'C:\Program Files\Git\bin\bash.exe'))
        if not bash.is_file():
            self.skipTest('Git Bash is required for generated shell checks')
        for failure in ('', 'source_hash', 'source_metadata', 'asset_hash', 'target_exists', 'copy'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as folder:
                base = Path(folder)
                root = base / 'root'
                source = root / EXPECTED['path'].lstrip('/')
                target = root / 'compat/lib64/libfile_migrate_init.z.so'
                asset = base / 'graphics/dipper/libfile_migrate_init.z.so'
                source.parent.mkdir(parents=True)
                target.parent.mkdir(parents=True)
                asset.parent.mkdir(parents=True)
                source.write_bytes(self.original if failure != 'source_hash' else b'bad input')
                asset.write_bytes(self.guarded if failure != 'asset_hash' else b'bad asset')
                if failure == 'target_exists':
                    target.write_bytes(b'keep existing')
                busybox = base / 'busybox'
                calls = base / 'calls'
                script = base / 'stage.sh'
                busybox.write_text('''#!/usr/bin/env bash
set -eu
command=$1
shift
printf '%s\\n' "$command $*" >> "$MOCK_CALLS"
case "$command" in
 stat)
  if [ "$MOCK_FAILURE" = source_metadata ] && [ "$3" = "$MOCK_SOURCE" ]; then
   echo 2000:0:644
  else echo 0:0:644; fi ;;
 chown) [ "$1" = 0:0 ] ;;
 chmod) [ "$1" = 0644 ]; /usr/bin/chmod "$@" ;;
 cp)
  if [ "$MOCK_FAILURE" = copy ]; then exit 43; fi
  /usr/bin/cp "$@" ;;
 mount)
  if [ "$1" = --bind ]; then /usr/bin/cp "$2" "$3"
  else [ "$1" = -o ] && [ "$2" = remount,bind,ro ]; fi ;;
 *) "/usr/bin/$command" "$@" ;;
esac
''', encoding='utf-8', newline='\n')
                unix = subprocess.run(
                    [str(bash), '-c', 'cygpath -u -- "$1"', '--', str(base)],
                    check=True, capture_output=True, text=True,
                ).stdout.strip()
                generated = stage(HEAD).replace(
                    'B=/busybox\nR=/hmos-root\n',
                    'B="$MOCK_BUSYBOX"\nR="$MOCK_ROOT"\n', 1,
                ).replace('/graphics/dipper/libfile_migrate_init.z.so', '"$MOCK_ASSET"')
                script.write_text(generated, encoding='utf-8', newline='\n')
                env = os.environ.copy()
                env.update({
                    'MOCK_BUSYBOX': unix + '/busybox', 'MOCK_ROOT': unix + '/root',
                    'MOCK_ASSET': unix + '/graphics/dipper/libfile_migrate_init.z.so',
                    'MOCK_SOURCE': unix + '/root' + EXPECTED['path'],
                    'MOCK_CALLS': unix + '/calls', 'MOCK_FAILURE': failure,
                })
                subprocess.run([str(bash), '-c', 'chmod +x -- "$1"', '--', str(busybox)],
                               check=True, capture_output=True)
                result = subprocess.run([str(bash), str(script)], env=env, capture_output=True, text=True)
                trace = calls.read_text(encoding='utf-8') if calls.exists() else ''
                if failure:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn('mount ', trace)
                    expected = self.original if failure != 'source_hash' else b'bad input'
                    self.assertEqual(source.read_bytes(), expected)
                    if failure == 'target_exists':
                        self.assertEqual(target.read_bytes(), b'keep existing')
                else:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn('FILE_MIGRATE_HOOK_GUARD_STAGED', result.stdout)
                    self.assertEqual(source.read_bytes(), self.guarded)
                    self.assertEqual(target.read_bytes(), self.guarded)
                    self.assertEqual(trace.count('mount '), 2)
                    self.assertIn('mount --bind ', trace)
                    self.assertIn('mount -o remount,bind,ro ', trace)


if __name__ == '__main__':
    unittest.main()

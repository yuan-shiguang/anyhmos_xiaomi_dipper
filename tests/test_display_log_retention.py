"""Exercise staged log retention with actual shell code and guarded builder input."""

import argparse
import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
TRACKED = ROOT / 'runtime/compat/graphics/apply-hardware-v116.sh'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
BEGIN = '# DIPPER_DISPLAY_LOG_RETENTION_BEGIN\n'
END = '# DIPPER_DISPLAY_LOG_RETENTION_END\n'
TARGETS = (
    ('dipper-brightness-stderr.txt', '5528:5528', '0600'),
    ('dipper-composer-metrics.txt', '3036:3036', '0640'),
    ('dipper-battery-hdi.txt', '3025:3025', '0600'),
    ('dipper-wifi-category-v134.txt', '3026:3026', '0600'),
    ('dipper-bluetooth-hdi.txt', '3021:3021', '0600'),
    ('dipper-bluetooth-check.txt', '2000:2000', '0600'),
)


def helpers():
    nodes = [
        node
        for node in TREE.body
        if isinstance(node, ast.FunctionDef)
        and node.name == 'enable_display_log_retention'
        or isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name) and target.id == 'DISPLAY_LOG_TARGETS'
            for target in node.targets
        )
    ]
    namespace = {}
    exec(
        compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), namespace
    )
    return namespace['enable_display_log_retention']


def legacy_script():
    return (
        'B=/busybox\nR=/hmos-root\necho HARDWARE_BEFORE\n'
        + ''.join(
            f': > $R/data/local/tmp/{name}\n'
            f'$B chown {owner} $R/data/local/tmp/{name}\n'
            f'$B chmod {mode} $R/data/local/tmp/{name}\n'
            for name, owner, mode in TARGETS
        )
        + 'echo HARDWARE_AFTER\n'
    )


class DisplayLogRetentionTests(unittest.TestCase):
    def setUp(self):
        self.tracked = TRACKED.read_text(encoding='utf-8')
        self.enable = helpers()
        self.generated = self.enable(legacy_script(), self.tracked)
        self.bash = shutil.which('bash') or r'C:\Program Files\Git\bin\bash.exe'
        self.temp = tempfile.TemporaryDirectory(
            prefix='display-log-', dir=os.environ.get('HMOS_HOST_TEST_WORK')
        )
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.logdir = self.work / 'root/data/local/tmp'
        self.logdir.mkdir(parents=True)
        self.busybox = self.work / 'busybox'
        self.busybox.write_text(
            '#!/bin/sh\n'
            'case "$1" in\n'
            ' cat) [ "${TEST_FAIL_READS:-0}" = 0 ] || exit 7; printf "%s\\n" "$TEST_BOOT_ID" ;;\n'
            ' cut) [ "${TEST_FAIL_READS:-0}" = 0 ] || exit 7; printf "123.45\\n" ;;\n'
            ' chown|chmod) printf "%s %s %s\\n" "$1" "$2" "$3" >> "$ACTION_LOG"; '
            '[ "${TEST_FAIL_PERMISSIONS:-0}" = 0 ] || exit 8 ;;\n'
            ' *) exit 9 ;;\n'
            'esac\n',
            encoding='utf-8',
            newline='\n',
        )
        self.busybox.chmod(0o755)

    def run_stage(self, source=None, **extra):
        source = self.generated if source is None else source
        block = source[source.index(BEGIN) : source.index(END) + len(END)]
        calls = '\n'.join(
            line
            for line in source.splitlines()
            if line.startswith('stage_hardware_log ')
        )
        script = (
            'set -eu\n'
            f'B="{self.busybox.as_posix()}"\nR="{(self.work / "root").as_posix()}"\n'
            + block
            + calls
            + '\necho STAGING_CONTINUED\n'
        )
        env = os.environ.copy()
        env.update(
            {
                'TEST_BOOT_ID': 'boot-one',
                'ACTION_LOG': str(self.work / 'actions').replace('\\', '/'),
            }
        )
        env.update(extra)
        return subprocess.run(
            [self.bash, '-s'], input=script, text=True, capture_output=True, env=env
        )

    def test_default_cli_is_disabled(self):
        nodes = [
            node
            for node in TREE.body
            if (
                isinstance(node, ast.Assign)
                and any(
                    isinstance(target, ast.Name) and target.id == 'parser'
                    for target in node.targets
                )
                or isinstance(node, ast.Expr)
                and isinstance(node.value, ast.Call)
                and isinstance(node.value.func, ast.Attribute)
                and isinstance(node.value.func.value, ast.Name)
                and node.value.func.value.id == 'parser'
                and node.value.func.attr == 'add_argument'
            )
        ]
        namespace = {
            'argparse': argparse,
            'pathlib': __import__('pathlib'),
            'REPO': ROOT,
            '__doc__': 'test',
        }
        exec(
            compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'),
            namespace,
        )
        args = namespace['parser'].parse_args(
            ['--donor', 'unused', '--original', 'unused', '--tools', 'unused',
             '--audio-build-dir', 'unused']
        )
        self.assertFalse(args.preserve_display_logs)

    def test_disabled_builder_branch_leaves_original_script_bytes(self):
        nodes = [
            node
            for node in TREE.body
            if isinstance(node, ast.Assign)
            and any(
                isinstance(target, ast.Name) and target.id == 'display_log_report'
                for target in node.targets
            )
            or isinstance(node, ast.If)
            and ast.unparse(node.test) == 'args.preserve_display_logs'
        ]
        original = legacy_script()
        namespace = {
            'hardware': original,
            'args': argparse.Namespace(preserve_display_logs=False),
        }
        exec(
            compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'),
            namespace,
        )
        self.assertEqual(namespace['hardware'].encode(), original.encode())
        self.assertEqual(namespace['display_log_report'], {'enabled': False})

    def test_preserves_old_bytes_and_records_boot_boundary(self):
        previous = b'old-boot\x00\xff no final newline'
        for name, _, _ in TARGETS:
            (self.logdir / name).write_bytes(previous)
        result = self.run_stage()
        self.assertEqual(result.returncode, 0, result.stderr)
        for name, owner, mode in TARGETS:
            content = (self.logdir / name).read_bytes()
            self.assertTrue(
                content.startswith(previous + b'\nDIPPER_DISPLAY_LOG_STAGE ')
            )
            self.assertRegex(content, rb'boot_id=boot-one uptime=123\.45 pid=\d+\n$')
            actions = (self.work / 'actions').read_text(encoding='utf-8')
            self.assertIn(f'chown {owner} ', actions)
            self.assertIn(f'chmod {mode} ', actions)
        self.assertIn('STAGING_CONTINUED', result.stdout)

    def test_missing_logs_created_and_repeat_staging_keeps_sessions(self):
        first = self.run_stage()
        self.assertEqual(first.returncode, 0, first.stderr)
        before = {name: (self.logdir / name).read_bytes() for name, _, _ in TARGETS}
        second = self.run_stage(TEST_BOOT_ID='boot-two')
        third = self.run_stage(TEST_BOOT_ID='boot-two')
        self.assertEqual((second.returncode, third.returncode), (0, 0))
        for name, _, _ in TARGETS:
            content = (self.logdir / name).read_bytes()
            self.assertTrue(content.startswith(before[name]))
            self.assertEqual(content.count(b'DIPPER_DISPLAY_LOG_STAGE '), 3)
            self.assertEqual(content.count(b'boot_id=boot-two '), 2)

    def test_permission_failures_are_nonfatal_and_do_not_truncate(self):
        name = TARGETS[0][0]
        (self.logdir / name).write_bytes(b'keep')
        result = self.run_stage(TEST_FAIL_PERMISSIONS='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.logdir / name).read_bytes().startswith(b'keep\n'))
        self.assertEqual(result.stderr.count('DIPPER_DISPLAY_LOG_CHOWN_FAILED='), 6)
        self.assertEqual(result.stderr.count('DIPPER_DISPLAY_LOG_CHMOD_FAILED='), 6)
        self.assertIn('STAGING_CONTINUED', result.stdout)

    def test_bad_paths_are_nonfatal_and_do_not_receive_permissions(self):
        name = TARGETS[0][0]
        (self.logdir / name).mkdir()
        result = self.run_stage()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('DIPPER_DISPLAY_LOG_INVALID_PATH=', result.stderr)
        self.assertNotIn(name, (self.work / 'actions').read_text(encoding='utf-8'))

    def test_append_and_clock_read_failures_do_not_abort_staging(self):
        shutil.rmtree(self.work / 'root')
        result = self.run_stage(TEST_FAIL_READS='1', TEST_FAIL_PERMISSIONS='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.count('DIPPER_DISPLAY_LOG_APPEND_FAILED='), 6)
        self.assertIn('STAGING_CONTINUED', result.stdout)

    def test_missing_clock_fields_use_unknown(self):
        result = self.run_stage(TEST_FAIL_READS='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        content = (self.logdir / TARGETS[0][0]).read_text(encoding='utf-8')
        self.assertIn('boot_id=unknown uptime=unknown pid=', content)

    def test_tracked_default_keeps_previous_truncate_behavior(self):
        for name, _, _ in TARGETS:
            (self.logdir / name).write_text('previous')
        result = self.run_stage(self.tracked)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(
            all((self.logdir / name).read_bytes() == b'' for name, _, _ in TARGETS)
        )

    def test_tracked_and_builder_enabled_logic_are_identical(self):
        begin = self.tracked.index(BEGIN)
        end = self.tracked.index(END) + len(END)
        expected = self.tracked[begin:end].replace(
            'PRESERVE_DISPLAY_LOGS=0\n', 'PRESERVE_DISPLAY_LOGS=1\n', 1
        )
        self.assertIn(expected, self.generated)
        self.assertEqual(self.generated.count('stage_hardware_log "'), 6)
        self.assertIn('echo HARDWARE_BEFORE\n', self.generated)
        self.assertTrue(self.generated.endswith('echo HARDWARE_AFTER\n'))

    def test_unknown_stage_input_or_changed_identity_is_rejected(self):
        legacy = legacy_script()
        for source in (
            legacy.replace('5528:5528', '0:0', 1),
            legacy + legacy,
            self.generated,
        ):
            with self.assertRaises(ValueError):
                self.enable(source, self.tracked)
        with self.assertRaises(ValueError):
            self.enable(
                legacy,
                self.tracked.replace(
                    'PRESERVE_DISPLAY_LOGS=0', 'PRESERVE_DISPLAY_LOGS=1'
                ),
            )

    def test_unrelated_hardware_failures_remain_fatal(self):
        block = self.generated[
            self.generated.index(BEGIN) : self.generated.index(END) + len(END)
        ]
        result = subprocess.run(
            [self.bash, '-s'],
            input='set -eu\nB=false\n' + block + 'false\necho UNREACHABLE\n',
            text=True,
            capture_output=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('UNREACHABLE', result.stdout)


if __name__ == '__main__':
    unittest.main()

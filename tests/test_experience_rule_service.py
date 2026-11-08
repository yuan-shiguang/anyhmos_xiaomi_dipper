import argparse
import ast
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
SPEC = importlib.util.spec_from_file_location(
    'experience_rule_service', ROOT / 'tools/experience_rule_service.py'
)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
COPY = '$B cp "$R/data/local/tmp/foundation-app-core.json" "$R/system/profile/foundation.json"\n'


class ExperienceRuleServiceTests(unittest.TestCase):
    def setUp(self):
        self.library = b'fixture original library\n'
        self.inputs = {'/system/lib64/liberms.z.so': self.library}
        self.pins = {
            name: hashlib.sha256(value).hexdigest() for name, value in self.inputs.items()
        }
        self.foundation = {
            'process': 'foundation',
            'systemability': [{'name': 401, 'keep': ['unchanged']}],
        }
        self.donor = {
            'process': 'foundation',
            'systemability': [copy.deepcopy(MODULE.EXPERIENCE_RULE_PROFILE)],
        }

    def test_option_is_off_and_requires_core_services(self):
        parser = argparse.ArgumentParser()
        declarations = [
            node for node in TREE.body
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
            and node.value.args and isinstance(node.value.args[0], ast.Constant)
            and node.value.args[0].value in (
                '--core-runtime-services', '--experience-rule-service'
            )
        ]
        guard = next(
            node for node in TREE.body if isinstance(node, ast.If)
            and ast.unparse(node.test)
            == 'args.experience_rule_service and (not args.core_runtime_services)'
        )
        exec(compile(ast.Module(body=declarations, type_ignores=[]), str(SOURCE), 'exec'),
             {'parser': parser})
        self.assertFalse(parser.parse_args([]).experience_rule_service)
        for values, accepted in [
            ([], True), (['--experience-rule-service'], False),
            (['--core-runtime-services', '--experience-rule-service'], True),
        ]:
            namespace = {'args': parser.parse_args(values), 'parser': parser}
            compiled = compile(ast.Module(body=[guard], type_ignores=[]), str(SOURCE), 'exec')
            with self.subTest(values=values):
                if accepted:
                    exec(compiled, namespace)
                else:
                    with self.assertRaises(SystemExit) as failure:
                        exec(compiled, namespace)
                    self.assertEqual(failure.exception.code, 2)

    # 核对原条目和输入完整性，拒绝重复或变更供体
    def test_selection_preserves_inputs_and_rejects_invalid_donor(self):
        before = copy.deepcopy((self.foundation, self.donor))
        with patch.dict(MODULE.EXPERIENCE_RULE_INPUTS, self.pins):
            selected = MODULE.add_experience_rule_service(
                self.foundation, self.donor, self.inputs
            )
            self.assertEqual(selected['systemability'][:-1], self.foundation['systemability'])
            self.assertEqual(selected['systemability'][-1], MODULE.EXPERIENCE_RULE_PROFILE)
            self.assertEqual((self.foundation, self.donor), before)
            for case in ('library', 'missing', 'duplicate', 'profile', 'process'):
                foundation, donor = copy.deepcopy(before)
                inputs = dict(self.inputs)
                if case == 'library':
                    inputs['/system/lib64/liberms.z.so'] += b'changed'
                elif case == 'missing':
                    inputs.clear()
                elif case == 'duplicate':
                    foundation['systemability'].append({'name': 6105})
                elif case == 'profile':
                    donor['systemability'][0]['extension'] = []
                else:
                    donor['process'] = 'other'
                with self.subTest(case=case), self.assertRaises(ValueError):
                    MODULE.add_experience_rule_service(foundation, donor, inputs)

    def test_unguarded_or_repeated_startup_is_rejected(self):
        for startup in (
            COPY, 'core_runtime_inputs_ready=1\n' + COPY * 2,
            'core_runtime_inputs_ready=1\n' + COPY + 'ERMS_PROFILE_SELECTED=1',
        ):
            with self.subTest(startup=startup), self.assertRaises(ValueError):
                MODULE.add_experience_rule_startup(startup, 'a' * 64, 'b' * 64)

    # 执行真实文件暂存和改名，失败时核对核心清单仍完整
    def test_runtime_failure_retains_core_and_success_selects_original_profile(self):
        bash = shutil.which('bash') or r'C:\Program Files\Git\bin\bash.exe'
        core = b'{"core":11}\n'
        candidate = b'{"core":11,"erms":6105}\n'
        core_sha = hashlib.sha256(core).hexdigest()
        selected_sha = hashlib.sha256(candidate).hexdigest()
        for mode in (
            'ok', 'library', 'profile', 'partial-copy', 'chown', 'move',
            'move-after-success', 'existing-temp',
        ):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                for directory in ('system/profile', 'system/lib64', 'data/local/tmp'):
                    (root / directory).mkdir(parents=True, exist_ok=True)
                (root / 'data/local/tmp/foundation-app-core.json').write_bytes(core)
                (root / 'system/lib64/liberms.z.so').write_bytes(
                    self.library + (b'changed' if mode == 'library' else b'')
                )
                source = root / 'candidate.json'
                source.write_bytes(candidate + (b'changed' if mode == 'profile' else b''))
                if mode == 'existing-temp':
                    (root / 'system/profile/.foundation-erms.tmp').write_bytes(b'keep')
                with patch.dict(MODULE.EXPERIENCE_RULE_INPUTS, self.pins):
                    generated = MODULE.add_experience_rule_startup(
                        'core_runtime_inputs_ready=1\nif true; then\n' + COPY + 'fi\n',
                        selected_sha, core_sha,
                    )
                generated = generated.replace(
                    '/graphics/dipper/foundation-experience-rule.json', source.as_posix()
                )
                shell = r'''set -eu
B=bb
bb() {
 case "$1" in
 chown) [ "$MODE" != chown ] ;;
 cp)
  if [ "$MODE" = partial-copy ] && [ "$3" = "$R/system/profile/.foundation-erms.tmp" ]; then
   printf partial > "$3"
   return 1
  fi
  command "$@"
  ;;
 mv)
  [ "$MODE" != move ] || return 1
  command "$@"
  [ "$MODE" != move-after-success ]
  ;;
 *) command "$@" ;;
 esac
}
'''
                result = subprocess.run(
                    [bash, '-s'], input=shell + generated, text=True,
                    capture_output=True, timeout=10,
                    env=dict(os.environ, R=root.as_posix(), MODE=mode),
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                selected = mode in ('ok', 'move-after-success')
                self.assertEqual(
                    (root / 'system/profile/foundation.json').read_bytes(),
                    candidate if selected else core,
                )
                self.assertIn(
                    'ERMS_PROFILE_SELECTED=6105' if selected else 'ERMS_CORE_PROFILE_RETAINED=1',
                    result.stdout,
                )
                if mode == 'existing-temp':
                    self.assertEqual(
                        (root / 'system/profile/.foundation-erms.tmp').read_bytes(), b'keep'
                    )

    @unittest.skipUnless(
        os.environ.get('HMOS_EXPERIENCE_RULE_DONOR_INPUTS'), 'private donor not supplied'
    )
    def test_actual_donor_reproduces_the_device_profile(self):
        donor_root = Path(os.environ['HMOS_EXPERIENCE_RULE_DONOR_INPUTS'])
        donor = json.loads((donor_root / 'system/profile/foundation.json').read_bytes())
        foundation = json.loads(Path(os.environ['HMOS_EXPERIENCE_RULE_BASELINE']).read_bytes())
        inputs = {
            path: (donor_root / path.lstrip('/')).read_bytes()
            for path in MODULE.EXPERIENCE_RULE_INPUTS
        }
        selected = MODULE.add_experience_rule_service(foundation, donor, inputs)
        encoded = (json.dumps(selected, indent=2) + '\n').encode()
        self.assertEqual(
            hashlib.sha256(encoded).hexdigest(),
            '09a39328ba7a049b7a9a3b0322e0909ddf4751c9e40738ad5effe8ae1fb3f2f5',
        )


if __name__ == '__main__':
    unittest.main()


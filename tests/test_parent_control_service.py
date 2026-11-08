import argparse
import ast
import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
BUILDER = ROOT / 'tools/build_dipper_ramdisk.py'
SPEC = importlib.util.spec_from_file_location(
    'parent_control_service', ROOT / 'tools/parent_control_service.py'
)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
HELPER = ROOT / 'runtime/compat/graphics/prepare-parentcontrol-el1.sh'
BASH = shutil.which('bash') or r'C:\Program Files\Git\bin\bash.exe'


def shell_path(path):
    value = path.as_posix()
    return '/' + value[0].lower() + value[2:] if path.drive else value


def run_shell(script, env):
    return subprocess.run(
        [BASH, '-s'], input=script, text=True, encoding='utf-8', errors='replace',
        capture_output=True, timeout=10, env=dict(os.environ, **env),
    )


class ParentControlServiceTests(unittest.TestCase):
    def setUp(self):
        self.service = {
            'name': 'parent_control_service',
            'path': ['/system/bin/sa_main', '/system/profile/parent_control_service.json'],
            'uid': 'parent_control_service',
            'gid': ['parent_control_service', 'shell', 'oeminfo_nvme_server'],
            'ondemand': True, 'apl': 'system_basic', 'secon': 'u:r:parent_control:s0',
            'sandbox': 0, 'permission': ['fixture.original.permission'],
            'permission_acls': ['fixture.original.permission'],
        }
        profile = {
            'process': 'parent_control_service',
            'systemability': [{
                'name': 65862, 'libpath': 'libparentcontrol_service.z.so',
                'run-on-create': False, 'auto-restart': True,
                'start-on-demand': {'bindercall': True, 'param': [{
                    'name': 'persist.parentcontrol.enable', 'value': 'true',
                }]},
                'stop-on-demand': {'param': [{
                    'name': 'persist.parentcontrol.enable', 'value': 'false',
                }]},
            }],
        }
        self.inputs = {name: ('fixture:' + name).encode()
                       for name in MODULE.PARENT_CONTROL_INPUTS}
        self.inputs['/system/etc/init/parentcontrolservice.cfg'] = json.dumps({
            'jobs': [{'name': 'boot', 'cmds': ['fixture original directory job']}],
            'services': [self.service],
        }).encode()
        self.inputs['/system/profile/parent_control_service.json'] = json.dumps(profile).encode()
        self.inputs['/system/etc/passwd'] = b'parent_control_service:x:7007:7007:::/bin/false\n'
        self.inputs['/system/etc/group'] = (
            b'parent_control_service:x:7007:\nshell:x:2000:\noeminfo_nvme_server:x:5004:\n'
        )
        self.pins = {name: MODULE.digest(data) for name, data in self.inputs.items()}
        self.desktop = {'jobs': [{'name': 'core-job', 'cmds': ['keep-core']}],
                        'services': [{'name': 'core-service', 'keep': [1]}]}
        self.init = {'import': ['/compat/etc/init-desktop-services.cfg'],
                     'jobs': [], 'services': []}

    def test_option_defaults_off_and_requires_both_dependencies(self):
        tree = ast.parse(BUILDER.read_text(encoding='utf-8'))
        names = ('--parent-control-service', '--core-runtime-services',
                 '--private-persist-parameters')
        declarations = [node for node in tree.body if isinstance(node, ast.Expr)
                        and isinstance(node.value, ast.Call) and node.value.args
                        and isinstance(node.value.args[0], ast.Constant)
                        and node.value.args[0].value in names]
        self.assertEqual(len(declarations), 3)
        parser = argparse.ArgumentParser()
        exec(compile(ast.Module(body=declarations, type_ignores=[]), str(BUILDER), 'exec'),
             {'parser': parser})
        self.assertFalse(parser.parse_args([]).parent_control_service)
        guards = [node for node in tree.body if isinstance(node, ast.If)
                  and any(isinstance(item, ast.Attribute) and item.attr == 'parent_control_service'
                          for item in ast.walk(node.test))
                  and any(isinstance(item, ast.Call) and isinstance(item.func, ast.Attribute)
                          and item.func.attr == 'error' for item in ast.walk(node))]
        self.assertTrue(guards)
        for core, private in ((False, False), (True, False), (False, True), (True, True)):
            values = ['--parent-control-service']
            if core:
                values.append('--core-runtime-services')
            if private:
                values.append('--private-persist-parameters')
            namespace = {'args': parser.parse_args(values), 'parser': parser}
            compiled = compile(ast.Module(body=guards, type_ignores=[]), str(BUILDER), 'exec')
            with self.subTest(core=core, private=private), contextlib.redirect_stderr(io.StringIO()):
                if core and private:
                    exec(compiled, namespace)
                else:
                    with self.assertRaises(SystemExit):
                        exec(compiled, namespace)

    def test_default_off_does_not_read_or_prepare_optional_inputs(self):
        tree = ast.parse(BUILDER.read_text(encoding='utf-8'))
        blocks = [node for node in tree.body if isinstance(node, ast.If) and (
            any(isinstance(item, ast.Attribute) and item.attr == 'parent_control_service'
                for item in ast.walk(node.test))
            or any(isinstance(item, ast.Name) and item.id == 'parent_control_payload'
                   for item in ast.walk(node.test))
        )]
        self.assertTrue(blocks)
        namespace = {'args': argparse.Namespace(parent_control_service=False),
                     'parent_control_payload': None}
        exec(compile(ast.Module(body=blocks, type_ignores=[]), str(BUILDER), 'exec'), namespace)
        self.assertNotIn('parent_control_inputs', namespace)
        self.assertNotIn('parent_control_cfg', namespace)

    def test_selection_preserves_original_identity_policy_and_inputs(self):
        original = copy.deepcopy((self.desktop, self.init, self.inputs))
        with patch.dict(MODULE.PARENT_CONTROL_INPUTS, self.pins, clear=True):
            selected = MODULE.select_parent_control_service(self.desktop, self.init, self.inputs)
        self.assertEqual(selected['jobs'][:-2], self.desktop['jobs'])
        self.assertEqual(selected['services'][:-1], self.desktop['services'])
        expected = dict(self.service, disabled=1, once=1, saspawn=False,
                        **{'start-mode': 'condition'})
        self.assertEqual(selected['services'][-1], expected)
        self.assertNotIn('caps', selected['services'][-1])
        self.assertEqual([job['cmds'] for job in selected['jobs'][-2:]],
                         [['start parent_control_service'], ['stop parent_control_service']])
        self.assertNotIn('persist.parentcontrol.enable', json.dumps(selected['jobs']))
        self.assertEqual((self.desktop, self.init, self.inputs), original)
        selected['services'][-1]['permission'].append('changed-only-in-result')
        self.assertEqual((self.desktop, self.init, self.inputs), original)

    def test_input_identity_and_on_demand_failures_are_refused(self):
        for mode in ('missing', 'hash', 'uid', 'groups', 'profile', 'caps'):
            inputs = dict(self.inputs)
            if mode == 'missing':
                inputs.pop('/system/lib64/libparentcontrol_service.z.so')
            elif mode == 'hash':
                inputs['/system/lib64/libparentcontrol_service.z.so'] += b'changed'
            elif mode == 'uid':
                inputs['/system/etc/passwd'] = b'parent_control_service:x:0:0:::/bin/false\n'
            elif mode == 'groups':
                inputs['/system/etc/group'] += b'shell:x:9999:\n'
            elif mode == 'profile':
                data = json.loads(inputs['/system/profile/parent_control_service.json'])
                data['systemability'][0]['run-on-create'] = True
                inputs['/system/profile/parent_control_service.json'] = json.dumps(data).encode()
            else:
                data = json.loads(inputs['/system/etc/init/parentcontrolservice.cfg'])
                data['services'][0]['caps'] = ['CAP_SYS_ADMIN']
                inputs['/system/etc/init/parentcontrolservice.cfg'] = json.dumps(data).encode()
            pins = self.pins if mode in ('missing', 'hash') else {
                name: MODULE.digest(data) for name, data in inputs.items()
            }
            with self.subTest(mode=mode), patch.dict(MODULE.PARENT_CONTROL_INPUTS, pins, clear=True):
                with self.assertRaises(ValueError):
                    MODULE.select_parent_control_service(self.desktop, self.init, inputs)

    def test_duplicate_import_service_gate_or_command_is_refused(self):
        for location in ('init', 'desktop'):
            for mode in ('import', 'underscore-import', 'wildcard', 'service', 'gate', 'command'):
                desktop, init = copy.deepcopy(self.desktop), copy.deepcopy(self.init)
                target = init if location == 'init' else desktop
                if mode in ('import', 'underscore-import', 'wildcard'):
                    target.setdefault('import', []).append({
                        'import': '/compat/etc/init-parentcontrol-ondemand.cfg',
                        'underscore-import': '/compat/etc/init-parent_control.cfg',
                        'wildcard': '/compat/etc/*.cfg',
                    }[mode])
                elif mode == 'service':
                    target['services'].append({'name': 'parent_control_service'})
                else:
                    target['jobs'].append({'name': 'param:dipper-parentcontrol-start'
                                           if mode == 'gate' else 'other-name',
                                           'cmds': ['start parent_control_service']
                                           if mode == 'command' else []})
                with self.subTest(location=location, mode=mode), patch.dict(
                    MODULE.PARENT_CONTROL_INPUTS, self.pins, clear=True
                ), self.assertRaises(ValueError):
                    MODULE.select_parent_control_service(desktop, init, self.inputs)

    def test_payload_preserves_raw_helper_and_runtime_prefix(self):
        with patch.dict(MODULE.PARENT_CONTROL_INPUTS, self.pins, clear=True):
            candidate, helper, report = MODULE.prepare_parent_control_payload(
                self.desktop, self.init, self.inputs, ROOT
            )
        self.assertEqual(helper, HELPER.read_bytes())
        self.assertEqual(helper, HELPER.read_bytes())
        self.assertEqual(json.loads(candidate)['services'][-1]['uid'], 'parent_control_service')
        self.assertFalse(report['runtime_validated'])
        runtime = 'set -eu\nB=/busybox\nR=/hmos-root\necho ORIGINAL_PREFIX\n'
        with patch.object(MODULE, 'PARENT_CONTROL_RUNTIME_SHA256', MODULE.digest(runtime.encode())):
            result = MODULE.add_parent_control_startup(runtime, '1' * 64, '2' * 64,
                                                       '3' * 64, '4' * 64)
            self.assertTrue(result.startswith(runtime))
            self.assertNotIn('set +e', result)
            with self.assertRaises(ValueError):
                MODULE.add_parent_control_startup(result, '1' * 64, '2' * 64,
                                                 '3' * 64, '4' * 64)
            with self.assertRaises(ValueError):
                MODULE.add_parent_control_startup(runtime, '1' * 64, '1' * 64,
                                                 '3' * 64, '4' * 64)

    def test_optional_tail_does_not_swallow_original_critical_failure(self):
        runtime = 'set -eu\necho ORIGINAL_CRITICAL_FAILURE >&2\nexit 61\n'
        with patch.object(MODULE, 'PARENT_CONTROL_RUNTIME_SHA256', MODULE.digest(runtime.encode())):
            selected = MODULE.add_parent_control_startup(
                runtime, '1' * 64, '2' * 64, '3' * 64, '4' * 64
            )
        self.assertTrue(selected.startswith(runtime))
        result = run_shell(selected + '\necho CORE_CONTINUED\n', {})
        self.assertEqual(result.returncode, 61)
        self.assertIn('ORIGINAL_CRITICAL_FAILURE', result.stderr)
        self.assertNotIn('PARENTCONTROL_PROFILE_', result.stdout + result.stderr)
        self.assertNotIn('CORE_CONTINUED', result.stdout)

    # 真实核文件和原子选择，设备属主与模式由明确替身提供
    def test_runtime_transaction_keeps_baseline_on_failures(self):
        modes = ('ok', 'private', 'profile', 'library', 'passwd', 'group', 'helper-hash',
                 'candidate-hash', 'helper-fail', 'target-unknown', 'target-links',
                 'target-owner', 'existing-temp', 'partial-copy', 'chown', 'chmod',
                 'temp-links', 'rename-fail', 'rename-after-success', 'rename-corrupt')
        for mode in modes:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                runtime, payload = root / 'runtime', root / 'payload'
                (runtime / 'compat/etc').mkdir(parents=True)
                payload.mkdir()
                target = runtime / 'compat/etc/init-desktop-services.cfg'
                temp = runtime / 'compat/etc/.init-desktop-parentcontrol.tmp'
                before = b'{"services":[{"name":"core"}],"jobs":[]}\n'
                after = b'{"services":[{"name":"core"},{"name":"parent_control_service"}],"jobs":[]}\n'
                target.write_bytes(before)
                candidate = payload / 'init-desktop-parentcontrol.cfg'
                candidate.write_bytes(after)
                helper = payload / 'prepare-parentcontrol-el1.sh'
                helper.write_bytes(b'fixture helper checked but isolated by stub\n')
                private = root / 'hmos-init-debug.cfg'
                private.write_bytes(b'fixture checked private configuration\n')
                called = root / 'helper-called'
                for name, data in self.inputs.items():
                    path = runtime / name.lstrip('/')
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                prefix = 'set -eu\nset -o pipefail\nB=bb\nR=' + shell_path(runtime) + '\n'
                with patch.dict(MODULE.PARENT_CONTROL_INPUTS, self.pins, clear=True), patch.object(
                    MODULE, 'PARENT_CONTROL_RUNTIME_SHA256', MODULE.digest(prefix.encode())
                ):
                    script = MODULE.add_parent_control_startup(
                        prefix, MODULE.digest(before), MODULE.digest(after),
                        MODULE.digest(helper.read_bytes()), MODULE.digest(private.read_bytes()),
                    )
                script = script.replace('/graphics/dipper', shell_path(payload)).replace(
                    '/hmos-init-debug.cfg', shell_path(private)
                )
                changes = {'private': private, 'helper-hash': helper, 'candidate-hash': candidate,
                           'profile': runtime / 'system/profile/parent_control_service.json',
                           'library': runtime / 'system/lib64/libparentcontrol_service.z.so',
                           'passwd': runtime / 'system/etc/passwd', 'group': runtime / 'system/etc/group'}
                if mode in changes:
                    changes[mode].write_bytes(changes[mode].read_bytes() + b'changed')
                elif mode == 'target-unknown':
                    target.write_bytes(b'unknown existing configuration')
                elif mode == 'target-links':
                    os.link(target, root / 'target-alias')
                elif mode == 'existing-temp':
                    temp.write_bytes(b'keep existing temporary bytes')
                initial = target.read_bytes()
                stub = r'''bb() {
 local command_name=$1
 shift
 case "$command_name" in
 stat)
  local format=$2 target=$3
  case "$format" in
  %u:%g:%a) echo 0:0:755;;
  %u:%g:%a:%h)
   if [ "$MODE" = target-owner ] && [ "$target" = "$R/compat/etc/init-desktop-services.cfg" ]; then
    echo 7:7:644:1
   else echo "0:0:644:$(command stat -c %h "$target")"; fi;;
  *) command stat "$@";;
  esac;;
 sh)
  printf called >> "$PARENT_TEST_CALLS"
  [ "$MODE" != helper-fail ];;
 cp)
  if [ "$MODE" = partial-copy ]; then printf partial > "$2"; return 1; fi
  command cp "$@" || return 1
  if [ "$MODE" = temp-links ]; then command ln "$2" "$2.alias"; fi;;
 chown) [ "$MODE" != chown ];;
 chmod) [ "$MODE" != chmod ] || return 1; command chmod "$@";;
 mv)
  [ "$MODE" != rename-fail ] || return 1
  if [ "$MODE" = rename-corrupt ]; then printf unknown > "$3"; return 1; fi
  command mv "$@" || return 1
  [ "$MODE" != rename-after-success ];;
 *) command "$command_name" "$@";;
 esac
}
'''
                result = run_shell(stub + script + '\necho CORE_CONTINUED\n',
                                   {'MODE': mode, 'PARENT_TEST_CALLS': shell_path(called)})
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('CORE_CONTINUED', result.stdout)
                output = result.stdout + result.stderr
                if mode in ('ok', 'rename-after-success'):
                    self.assertEqual(target.read_bytes(), after)
                    self.assertIn('PARENTCONTROL_PROFILE_SELECTED=65862', output)
                elif mode == 'rename-corrupt':
                    self.assertEqual(target.read_bytes(), b'unknown')
                    self.assertIn('UNKNOWN', output)
                    self.assertNotIn('BASELINE_RETAINED', output)
                else:
                    self.assertEqual(target.read_bytes(), initial)
                    self.assertNotIn('PARENTCONTROL_PROFILE_SELECTED=', output)
                    if mode in ('target-unknown', 'target-links', 'target-owner'):
                        self.assertIn('UNKNOWN', output)
                        self.assertNotIn('BASELINE_RETAINED', output)
                    else:
                        self.assertIn('BASELINE_RETAINED', output)
                if mode in changes or mode in ('target-unknown', 'target-links', 'target-owner'):
                    self.assertFalse(called.exists())
                if mode == 'existing-temp':
                    self.assertEqual(temp.read_bytes(), b'keep existing temporary bytes')

    # 原目录脚本只在已有 EL2 身份通过后创建缺失 EL1
    def test_original_data_helper_never_creates_el2_or_repairs_existing_el1(self):
        source = HELPER.read_text(encoding='utf-8')
        for mode in ('missing-el2', 'bad-el2', 'bad-el1', 'mkdir-fail', 'ready'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                parent = root / 'data/service/el1/public'
                parent.mkdir(parents=True)
                el2 = root / 'data/service/el2/100/parentcontrol'
                if mode != 'missing-el2':
                    el2.mkdir(parents=True)
                    (el2 / 'policy.keep').write_bytes(b'original policy bytes')
                    inode = (el2.stat().st_dev, el2.stat().st_ino)
                if mode == 'bad-el1':
                    (parent / 'parentcontrol').mkdir()
                    (parent / 'parentcontrol/keep.log').write_bytes(b'original log')
                script = source.replace('B=/busybox', 'B=bb').replace(
                    'R=/hmos-root', 'R=' + shell_path(root)
                )
                stub = r'''bb() {
 local command_name=$1
 shift
 case "$command_name" in
 id) echo 0;;
 test) if [ "$1" = -b ]; then return 0; fi; command test "$@";;
 readlink) case "$2" in /dev/block/by-name/userdata) echo /dev/block/sda29;; *) command readlink "$@";; esac;;
 grep) return 0;;
 cat) echo 8:29;;
 awk) return 0;;
 stat)
  local format=$2 target=$3 owner=0 mode=700
  case "$target" in */parentcontrol|*/parentcontrol/*) owner=7007;; esac
  case "$target" in */el2/100/parentcontrol) mode=711; [ "$MODE" != bad-el2 ] || owner=0;; esac
  if [ "$MODE" = bad-el1 ] && [ "$target" = "$R/data/service/el1/public/parentcontrol" ]; then owner=42; fi
  case "$format" in
  %u:%g) echo 0:0;;
  %u:%g:%a) echo "$owner:$owner:$mode";;
  %d:%i:%u:%g:%a) printf '%s:%s:%s:%s\n' "$(command stat -c '%d:%i' "$target")" "$owner" "$owner" "$mode";;
  *) command stat "$@";;
  esac;;
 mkdir)
  if [ "$MODE" = mkdir-fail ]; then case "$3" in */databases) return 1;; esac; fi
  if [ "$HMOS_TEST_HOST_METADATA" = windows ] && [ "$1" = -m ]; then
   [ "$2" = 0700 ] || return 1
   shift 2
  fi
  command mkdir "$@";;
 chown) return 0;;
 *) command "$command_name" "$@";;
 esac
}
'''
                result = run_shell(stub + script, {
                    'MODE': mode, 'HMOS_TEST_HOST_METADATA': 'windows' if os.name == 'nt' else 'posix',
                })
                self.assertEqual(result.returncode == 0, mode == 'ready', result.stderr)
                if mode == 'missing-el2':
                    self.assertFalse(el2.exists())
                    self.assertFalse((parent / 'parentcontrol').exists())
                else:
                    self.assertEqual((el2.stat().st_dev, el2.stat().st_ino), inode)
                    self.assertEqual((el2 / 'policy.keep').read_bytes(), b'original policy bytes')
                if mode == 'bad-el1':
                    self.assertEqual((parent / 'parentcontrol/keep.log').read_bytes(), b'original log')
                    self.assertFalse((parent / 'parentcontrol/databases').exists())
                if mode == 'ready':
                    self.assertTrue((parent / 'parentcontrol/cccs_grs/router').is_dir())

    @unittest.skipUnless(os.environ.get('HMOS_PARENT_CONTROL_DONOR_INPUTS'), 'private donor not supplied')
    def test_actual_donor_reproduces_reviewed_service_and_helper(self):
        donor = Path(os.environ['HMOS_PARENT_CONTROL_DONOR_INPUTS'])
        inputs = {name: (donor / name.lstrip('/')).read_bytes()
                  for name in MODULE.PARENT_CONTROL_INPUTS}
        cfg, helper, report = MODULE.prepare_parent_control_payload(
            {'jobs': [], 'services': []}, {'jobs': [], 'services': []}, inputs, ROOT
        )
        self.assertEqual(MODULE.digest(cfg),
                         'fadd23bef6e85e5184958963de98834f940af8baa74e739f3b0591b4de1c7069')
        self.assertEqual(MODULE.digest(helper),
                         '26868f75a37a05678d4d85a7e45a932260f897ce5b209e392b6dd06e7c628546')
        self.assertFalse(report['runtime_validated'])
        if os.environ.get('HMOS_PARENT_CONTROL_BASE_INIT'):
            baseline = json.loads(Path(os.environ['HMOS_PARENT_CONTROL_BASE_INIT']).read_bytes())
            MODULE.validate_parent_control_init(baseline)
        if os.environ.get('HMOS_PARENT_CONTROL_C1_INIT'):
            current = json.loads(Path(os.environ['HMOS_PARENT_CONTROL_C1_INIT']).read_bytes())
            with self.assertRaises(ValueError):
                MODULE.validate_parent_control_init(current)


if __name__ == '__main__':
    unittest.main()

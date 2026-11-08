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
import tarfile
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/build_dipper_ramdisk.py'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))
SPEC = importlib.util.spec_from_file_location(
    'hiview_event_service', ROOT / 'tools/hiview_event_service.py'
)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
LAUNCHER_SOURCE = ROOT / 'compat/services/hiview_namespace_launcher.c'
TEMPLATE = ROOT / 'runtime/compat/graphics/prepare-native-hiview-events.sh'
BASH = shutil.which('bash') or r'C:\Program Files\Git\bin\bash.exe'


def shell_path(path):
    value = path.as_posix()
    return '/' + value[0].lower() + value[2:] if path.drive else value


class NativeHiViewEventsTests(unittest.TestCase):
    def setUp(self):
        service = {
            'name': 'hiview', 'uid': 'hiview', 'gid': list(MODULE.HIVIEW_GROUPS),
            'path': ['/system/bin/hiview'], 'start-mode': 'boot',
            'jobs': {'on-start': 'services:hiview'},
            'caps': ['SYSLOG', 'CAP_NET_RAW', 'CAP_SYS_PTRACE', 'CAP_DAC_READ_SEARCH',
                     'CAP_DAC_OVERRIDE', 'CAP_KILL', 'CAP_CHOWN'],
            'permission': ['fixture.permission'], 'socket': [{'name': 'fixture'}],
        }
        self.inputs = {name: ('fixture:' + name).encode() for name in MODULE.HIVIEW_INPUTS}
        self.inputs['/system/etc/init/hiview.cfg'] = json.dumps({'services': [service]}).encode()
        self.inputs['/system/etc/passwd'] = b'hiview:x:1201:1201:::/bin/false\n'
        self.inputs['/system/etc/group'] = ''.join(
            name + ':x:' + number + ':\n' for name, number in MODULE.HIVIEW_GROUPS.items()
        ).encode()
        self.pins = {name: MODULE.digest(data) for name, data in self.inputs.items()}
        self.launcher = b'fixture launcher\n'
        self.desktop = {'services': [{'name': 'core', 'keep': [1]}],
                        'jobs': [{'name': 'core-job', 'cmds': ['keep']}], 'import': ['keep']}

    def payload(self):
        with patch.dict(MODULE.HIVIEW_INPUTS, self.pins, clear=True):
            archive, files = MODULE.build_hiview_assets(self.inputs, LAUNCHER_SOURCE.read_bytes())
        with patch.object(MODULE, 'HIVIEW_LAUNCHER_BINARY', MODULE.digest(self.launcher)):
            helper = MODULE.build_hiview_helper(TEMPLATE.read_text(encoding='utf-8'), archive, files)
        return archive, files, helper

    def test_option_defaults_off_and_requires_core(self):
        parser = argparse.ArgumentParser()
        declarations = [node for node in TREE.body if isinstance(node, ast.Expr)
                        and isinstance(node.value, ast.Call) and node.value.args
                        and isinstance(node.value.args[0], ast.Constant)
                        and node.value.args[0].value in (
                            '--core-runtime-services', '--native-hiview-events')]
        guard = next(node for node in TREE.body if isinstance(node, ast.If)
                     and ast.unparse(node.test)
                     == 'args.native_hiview_events and (not args.core_runtime_services)')
        exec(compile(ast.Module(body=declarations, type_ignores=[]), str(SOURCE), 'exec'),
             {'parser': parser})
        self.assertFalse(parser.parse_args([]).native_hiview_events)
        for values, accepted in [([], True), (['--native-hiview-events'], False),
                                 (['--native-hiview-events', '--core-runtime-services'], True)]:
            with self.subTest(values=values):
                namespace = {'args': parser.parse_args(values), 'parser': parser}
                compiled = compile(ast.Module(body=[guard], type_ignores=[]), str(SOURCE), 'exec')
                if accepted:
                    exec(compiled, namespace)
                else:
                    with self.assertRaises(SystemExit):
                        exec(compiled, namespace)

    def test_default_off_never_reads_or_builds_optional_payload(self):
        blocks = [node for node in TREE.body if isinstance(node, ast.If)
                  and ast.unparse(node.test) in ('args.native_hiview_events', 'hiview_payload')]
        self.assertEqual(len(blocks), 3)
        namespace = {'args': argparse.Namespace(native_hiview_events=False),
                     'hiview_payload': None}
        exec(compile(ast.Module(body=blocks, type_ignores=[]), str(SOURCE), 'exec'), namespace)
        self.assertNotIn('hiview_inputs', namespace)
        self.assertNotIn('hiview_launcher', namespace)

    def test_manual_payload_has_no_boot_preparation_hook(self):
        self.assertFalse(hasattr(MODULE, 'add_hiview_preparation'))
        self.assertNotIn('--install', TEMPLATE.read_text(encoding='utf-8'))
        blocks = [node for node in TREE.body if isinstance(node, ast.If)
                  and ast.unparse(node.test) in ('args.native_hiview_events', 'hiview_payload')]
        for node in blocks:
            self.assertFalse(any(isinstance(item, ast.Name) and item.id == 'userdata'
                                 for item in ast.walk(node)))

    def test_existing_native_import_or_startup_is_not_merged_twice(self):
        baseline = {'import': ['/compat/etc/init-desktop-services.cfg'],
                    'jobs': [], 'services': []}
        MODULE.validate_hiview_init(baseline)
        for mode in ('import', 'wildcard', 'service', 'startup'):
            candidate = copy.deepcopy(baseline)
            if mode == 'import':
                candidate['import'].append('/compat/etc/init-hiview-namespace.cfg')
            elif mode == 'wildcard':
                candidate['import'].append('/system/etc/init/*.cfg')
            elif mode == 'service':
                candidate['services'].append({'name': 'hiview'})
            else:
                candidate['jobs'].append({'name': 'other', 'cmds': ['start hiview']})
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                MODULE.validate_hiview_init(candidate)

    # 保留已有服务、原生身份和输入字节，拒绝重复或混用供体
    def test_native_selection_and_input_guards(self):
        before = copy.deepcopy((self.desktop, self.inputs))
        with patch.dict(MODULE.HIVIEW_INPUTS, self.pins, clear=True):
            selected = MODULE.select_hiview_service(self.desktop, self.inputs)
            self.assertEqual(selected['services'][:-1], self.desktop['services'])
            self.assertEqual(selected['jobs'][:-2], self.desktop['jobs'])
            self.assertEqual(selected['import'], ['keep'])
            original = json.loads(self.inputs['/system/etc/init/hiview.cfg'])['services'][0]
            service = selected['services'][-1]
            for name in ('uid', 'gid', 'permission', 'socket'):
                self.assertEqual(service[name], original[name])
            self.assertEqual(service['caps'], original['caps'] + ['CAP_SYS_ADMIN', 'CAP_SETPCAP'])
            self.assertNotIn('jobs', service)
            self.assertEqual((service['disabled'], service['once']), (1, 1))
            self.assertEqual((self.desktop, self.inputs), before)
            for mode in ('missing', 'changed', 'duplicate', 'gate'):
                inputs, desktop = dict(self.inputs), copy.deepcopy(self.desktop)
                if mode == 'missing':
                    inputs.pop('/system/etc/hiview/hisysevent.zip')
                elif mode == 'changed':
                    inputs['/system/etc/hiview/hisysevent.zip'] += b'changed'
                elif mode == 'duplicate':
                    desktop['services'].append({'name': 'hiview'})
                else:
                    desktop['jobs'].append({'name': 'param:dipper-native-hiview-events'})
                with self.subTest(mode=mode), self.assertRaises(ValueError):
                    MODULE.select_hiview_service(desktop, inputs)

    def test_asset_bytes_and_original_launcher_are_preserved(self):
        archive, files, helper = self.payload()
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'assets.tar.gz'
            target.write_bytes(archive)
            with tarfile.open(target) as package:
                self.assertEqual(sorted(package.getnames()), sorted(files))
                for entry in package:
                    self.assertTrue(entry.isfile())
                    self.assertFalse(entry.name.startswith('/') or '..' in entry.name.split('/'))
                    self.assertEqual(package.extractfile(entry).read(), files[entry.name])
        for name, data in self.inputs.items():
            if name.startswith('/system/etc/hiview/'):
                self.assertEqual(files['config-tree/' + name.removeprefix('/system/etc/hiview/')], data)
        self.assertEqual(MODULE.digest(LAUNCHER_SOURCE.read_bytes()), MODULE.HIVIEW_LAUNCHER_SOURCE)
        self.assertNotIn(b'LD_PRELOAD', helper)
        self.assertNotIn(b'DIPPER_RDB', helper)
        self.assertNotIn(b'setparam', helper)

    # 真实执行文件操作，设备属主和模式元数据由替身提供
    def test_preparation_failure_retains_request_and_fresh_run_is_required(self):
        archive, _, helper = self.payload()
        for mode in ('ok', 'archive', 'copy', 'unpack', 'run-owner', 'request-move',
                     'existing-run', 'existing-request-temp', 'invalid-nonce',
                     'long-nonce', 'launcher', 'launcher-temp', 'request-links',
                     'mount', 'alias', 'request-move-after', 'asset-chmod', 'directory-chmod',
                     'fresh-start', 'not-ready'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                runtime, payload = root / 'runtime', root / 'payload'
                for directory in ('compat/bin', 'data/local/tmp/dipper-hiview-events-v1',
                                  'data/system', 'data/log', 'log'):
                    (runtime / directory).mkdir(parents=True, exist_ok=True)
                payload.mkdir()
                (payload / 'hiview_namespace_launcher').write_bytes(self.launcher)
                (payload / 'hiview-events-assets.tar.gz').write_bytes(archive)
                request = runtime / 'data/local/tmp/dipper-hiview-events-v1/request'
                request.write_bytes(b'abcd\n')
                mountinfo = root / 'mountinfo'
                mountinfo.write_bytes(b'')
                script = helper.decode().replace('B=/busybox', 'B=bb').replace(
                    'R=/hmos-root', 'R=' + shell_path(runtime)
                ).replace('P=/graphics/dipper', 'P=' + shell_path(payload)).replace(
                    '/busybox chmod', 'chmod'
                ).replace('/proc/self/mountinfo', shell_path(mountinfo))
                stub = r'''bb() {
 command_name=$1
 shift
 case "$command_name" in
 chown) return 0;;
 mkdir)
  if [ "$HMOS_TEST_HOST_METADATA" = windows ] && [ "$1" = -m ]; then
   case "$2" in 0700|0755) shift 2;; *) return 1;; esac
  fi
  command mkdir "$@";;
 stat)
  format=$2
  target=$3
  owner=0
  case "$target" in */run-*|*/data/system/hiview) owner=1201;; */data/log) owner=1000;; esac
  [ "$MODE" != run-owner ] || case "$target" in */run-12) owner=42;; esac
  case "$format" in
  %u) echo "$owner";;
  %a) if [ -f "$target" ]; then echo 400; else echo 750; fi;;
  %u:%g:%a)
   case "$target" in */run-*) echo 1201:1000:700;; *) echo 0:0:755;; esac;;
  %u:%g:%a:%h)
   mode=600
   case "$target" in */hiview_namespace_launcher) mode=755;; esac
   echo "0:0:$mode:$(command stat -c %h "$target")";;
  %d:%i)
   if [ "$MODE" = alias ]; then echo 1:2; else command stat "$@"; fi;;
  *) command stat "$@";;
  esac;;
 cp)
  if [ "$MODE" = copy ]; then printf partial > "$2"; return 1; fi
  command cp "$@";;
 chmod)
  if [ "$MODE" = asset-chmod ] && [ "$1" = 0400 ]; then return 1; fi
  if [ "$MODE" = directory-chmod ] && [ "$1" = 0750 ]; then return 1; fi
  command chmod "$@";;
 tar) [ "$MODE" != unpack ] || return 1; command tar "$@";;
 mv)
  if [ "$MODE" = request-move ]; then
   case "$3" in */request) return 1;; esac
  fi
  command mv "$@" || return 1
  if [ "$MODE" = request-move-after ]; then
   case "$3" in */request) return 1;; esac
  fi;;
 *) command "$command_name" "$@";;
 esac
}
'''
                env = dict(os.environ, MODE='ok',
                           HMOS_TEST_HOST_METADATA='windows' if os.name == 'nt' else 'posix')
                (runtime / 'compat/bin/hiview_namespace_launcher').write_bytes(self.launcher)
                if mode == 'archive':
                    (payload / 'hiview-events-assets.tar.gz').write_bytes(archive + b'changed')
                elif mode == 'copy':
                    (runtime / 'compat/bin/hiview_namespace_launcher').unlink()
                elif mode == 'existing-run':
                    (request.parent / 'run-12').mkdir()
                    (request.parent / 'run-12/keep.log').write_bytes(b'keep')
                elif mode == 'existing-request-temp':
                    (request.parent / 'request-12.tmp').write_bytes(b'keep')
                elif mode == 'launcher':
                    (runtime / 'compat/bin/hiview_namespace_launcher').write_bytes(b'unknown')
                elif mode == 'launcher-temp':
                    (runtime / 'compat/bin/hiview_namespace_launcher').unlink()
                    (runtime / 'compat/bin/.hiview_namespace_launcher.tmp').write_bytes(b'keep')
                elif mode == 'request-links':
                    os.link(request, request.parent / 'request-alias')
                elif mode == 'mount':
                    mountinfo.write_text('1 0 0:1 / ' + shell_path(request.parent / 'run-12')
                                         + ' rw - tmpfs tmpfs rw\n', encoding='utf-8')
                elif mode in ('invalid-nonce', 'long-nonce'):
                    (runtime / 'compat/bin/hiview_namespace_launcher').unlink()
                elif mode == 'fresh-start':
                    request.unlink()
                    request.parent.rmdir()
                    (runtime / 'compat/bin/hiview_namespace_launcher').unlink()
                elif mode == 'not-ready':
                    (runtime / 'compat/bin/hiview_namespace_launcher').unlink()
                    (runtime / 'compat/bin').rmdir()
                    (runtime / 'compat').rmdir()
                invocation = '12'
                if mode == 'invalid-nonce':
                    invocation = 'ABC'
                elif mode == 'long-nonce':
                    invocation = 'a' * 33
                result = subprocess.run([BASH, '-s', '--', invocation], input=stub + script,
                                        text=True, capture_output=True, timeout=10,
                                        env=dict(env, MODE=mode))
                if mode in ('ok', 'request-move-after', 'fresh-start'):
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(request.read_bytes(), b'12\n')
                    self.assertEqual(
                        (runtime / 'compat/bin/hiview_namespace_launcher').read_bytes(), self.launcher
                    )
                    self.assertIn('HIVIEW_EVENTS_PREPARED', result.stdout)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(request.read_bytes(), b'abcd\n')
                    self.assertNotIn('HIVIEW_EVENTS_PREPARED', result.stdout)
                    if mode == 'launcher-temp':
                        self.assertEqual(
                            (runtime / 'compat/bin/.hiview_namespace_launcher.tmp').read_bytes(),
                            b'keep',
                        )
                    elif mode == 'existing-request-temp':
                        self.assertEqual((request.parent / 'request-12.tmp').read_bytes(), b'keep')
                    elif mode == 'existing-run':
                        self.assertEqual((request.parent / 'run-12/keep.log').read_bytes(), b'keep')
                    elif mode == 'copy':
                        self.assertFalse((runtime / 'compat/bin/hiview_namespace_launcher').exists())
                        self.assertEqual(
                            (runtime / 'compat/bin/.hiview_namespace_launcher.tmp').read_bytes(),
                            b'partial',
                        )
                    elif mode in ('invalid-nonce', 'long-nonce', 'not-ready'):
                        self.assertFalse((runtime / 'compat/bin/hiview_namespace_launcher').exists())
                        self.assertFalse((runtime / 'compat/bin/.hiview_namespace_launcher.tmp').exists())
                    if mode in ('archive', 'unpack', 'run-owner', 'request-move',
                                'existing-run', 'existing-request-temp', 'mount', 'alias',
                                'asset-chmod', 'directory-chmod'):
                        self.assertIn('HIVIEW_EVENTS_OLD_REQUEST_RETAINED', result.stderr)

    @unittest.skipUnless(os.environ.get('HMOS_HIVIEW_DONOR_INPUTS'), 'private donor not supplied')
    def test_actual_donor_reproduces_reviewed_config_assets_and_static_launcher(self):
        donor = Path(os.environ['HMOS_HIVIEW_DONOR_INPUTS'])
        inputs = {name: (donor / name.lstrip('/')).read_bytes() for name in MODULE.HIVIEW_INPUTS}
        selected = MODULE.select_hiview_service({'jobs': [], 'services': []}, inputs)
        encoded = (json.dumps(selected, indent=2) + '\n').encode()
        self.assertEqual(MODULE.digest(encoded),
                         '196e867f8c6fd1cc559733d8abc22bc1b19e63d84f43409c3f4fe5714900f92d')
        archive, _ = MODULE.build_hiview_assets(inputs, LAUNCHER_SOURCE.read_bytes())
        self.assertEqual(MODULE.digest(archive),
                         'f0906bbf493370916ce4ea03734e9fed0904cba8f3c36e44a237bd8c95102632')
        with tempfile.TemporaryDirectory() as temporary:
            prepared, payload, report = MODULE.prepare_hiview_payload(
                {'jobs': [], 'services': []}, inputs, ROOT,
                Path(os.environ['HMOS_HIVIEW_TOOLS']), Path(temporary),
            )
        self.assertEqual(prepared, selected)
        self.assertEqual(payload['hiview-events-assets.tar.gz'][0], archive)
        self.assertEqual(MODULE.digest(payload['hiview_namespace_launcher'][0]),
                         MODULE.HIVIEW_LAUNCHER_BINARY)
        self.assertEqual(payload['hiview_namespace_launcher'][1], 0o100755)
        self.assertFalse(report['started_by_default'])
        self.assertFalse(report['sql_tracing_included'])
        if os.environ.get('HMOS_HIVIEW_BASELINE_DESKTOP'):
            baseline = json.loads(Path(os.environ['HMOS_HIVIEW_BASELINE_DESKTOP']).read_bytes())
            selected = MODULE.select_hiview_service(baseline, inputs)
            self.assertEqual(selected['services'][:-1], baseline['services'])
            self.assertEqual(selected['jobs'][:-2], baseline['jobs'])
            self.assertEqual(
                {name: value for name, value in selected.items() if name not in ('jobs', 'services')},
                {name: value for name, value in baseline.items() if name not in ('jobs', 'services')},
            )



if __name__ == '__main__':
    unittest.main()

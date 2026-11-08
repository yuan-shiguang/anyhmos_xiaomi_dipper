import ast
import copy
import gzip
import hashlib
import io
import json
import pathlib
import re
import subprocess
import tarfile


HIVIEW_LAUNCHER_SOURCE = 'f2160a95e5ffb0985ef11df8ad37febfcda15ad92b4717f857cdf9a915997079'
HIVIEW_LAUNCHER_BINARY = '00b4c6a66d80878465335f915cab3b3f5d7019e9a247a51b79975a0c34d019e0'
HIVIEW_INPUTS = {
    '/system/etc/init/hiview.cfg': 'b01fa998e9d437287664b606a13712e7d161ab7c297401e815cdade193b868f7',
    '/system/bin/hiview': 'ab6f4ddc9f44a0ff8d6db8ce967e10a045a49342a133b6cc2df96554866abda5',
    '/system/lib64/libadft.z.so': '24efe1509539d38cab8014a9152abeb938019c869fd158b189923b2eeba8770c',
    '/system/etc/passwd': '4e70786ac3c6ace840da60e0608ef4424c9963298241c827a425c69705abc9b9',
    '/system/etc/group': '7c078f47d90b41127e6be365318e072c7639dc99a4073d5ab7a8d0f4aa68cb55',
    '/system/etc/hiview/hiview_platform_config': '0fe2554db804ed4dcf01a0cca539fea71241379935acf78fd85e52f4cdc5e0db',
    '/system/etc/hiview/event_store_config.json': 'a52f96932fa0164e6fdc3570101a3e58445e50cdd2143d092444d21fa71adda0',
    '/system/etc/hiview/hisysevent.zip': 'a5d25a4c6c6ff016309da84dc9ed766ecb6a38c610d6952c22c12d25a38612ed',
    '/system/etc/hiview/event_threshold.json': '0e3bb6a42476fd8a248a8de898c9df66432dab199291bacd8e930ac145458a7f',
    '/system/etc/hiview/log_type.json': '2f0338ffdb5418a431b84cf16aea2542c79a5d382d692278cb23c61e7e997eda',
    '/system/etc/hiview/hiview_config_version': 'c01e5d7983d7d09851a265b417e41808fdb11bce16c7ddfade7a95da38502a82',
    '/system/etc/hiview/hwkey_param_upgrade_hiviewdfx_v1.pem': 'e0757b9d5798bf76e258d37ae8309832bc392523ff2c337349a07219ab8df29e',
    '/system/etc/hiview/dispatch_rule/usageEventPipeline': '9ee729571a08eb620cb9b3a85940185fd25bbc0942146c4cf813c7e32e358c0a',
}
HIVIEW_GROUPS = {
    'system': '1000', 'log': '1007', 'hiview': '1201', 'readproc': '3009',
    'file_manager': '1006', 'hmi_host': '3096', 'radio': '1001',
    'shader_cache': '3099', 'shell': '2000',
}
HIVIEW_JOB_NAMES = ('param:dipper-native-hiview-events', 'param:dipper-stop-native-hiview-events')


def digest(data):
    return hashlib.sha256(data).hexdigest()


# 拒绝既有事件服务或导入入口，避免重复定义和隐式启动
def validate_hiview_init(config):
    if any('hiview' in path.lower() or '*' in path for path in config.get('import', [])):
        raise ValueError('Native HiView import already exists or needs review')
    if any(service['name'] == 'hiview' for service in config.get('services', [])) or any(
        job['name'] in HIVIEW_JOB_NAMES or any(
            command.split()[:2] in (['start', 'hiview'], ['stop', 'hiview'])
            for command in job.get('cmds', [])
        )
        for job in config.get('jobs', [])
    ):
        raise ValueError('Native HiView definition or startup already exists')


# 校验原服务、事件定义及用户身份，拒绝混用供体
def validate_hiview_inputs(inputs):
    if set(inputs) != set(HIVIEW_INPUTS):
        raise ValueError('Unexpected native HiView input set')
    for name, expected in HIVIEW_INPUTS.items():
        if digest(inputs[name]) != expected:
            raise ValueError('Native HiView donor hash mismatch: ' + name)
    users = [line.split(':') for line in inputs['/system/etc/passwd'].decode().splitlines()]
    selected = [line for line in users if line[0] == 'hiview']
    groups = {
        line.split(':')[0]: line.split(':')[2]
        for line in inputs['/system/etc/group'].decode().splitlines()
        if line.split(':')[0] in HIVIEW_GROUPS
    }
    if len(selected) != 1 or selected[0][2:4] != ['1201', '1201'] or groups != HIVIEW_GROUPS:
        raise ValueError('Native HiView donor UID or GID mismatch')


# 只替换原服务入口，保留原生权限、套接字和访问令牌配置
def select_hiview_service(desktop, inputs):
    validate_hiview_inputs(inputs)
    validate_hiview_init(desktop)
    original = json.loads(inputs['/system/etc/init/hiview.cfg'])
    if len(original['services']) != 1:
        raise ValueError('Unexpected native HiView service count')
    service = copy.deepcopy(original['services'][0])
    if (
        service['name'] != 'hiview' or service['uid'] != 'hiview'
        or service['path'] != ['/system/bin/hiview']
        or service['gid'] != list(HIVIEW_GROUPS)
        or service['jobs'] != {'on-start': 'services:hiview'}
        or service['start-mode'] != 'boot'
    ):
        raise ValueError('Native HiView service differs')
    names = HIVIEW_JOB_NAMES
    service.update({'path': ['/compat/bin/hiview_namespace_launcher'],
                    'disabled': 1, 'once': 1, 'start-mode': 'condition'})
    service.pop('jobs')
    service['caps'] += ['CAP_SYS_ADMIN', 'CAP_SETPCAP']
    result = copy.deepcopy(desktop)
    result['services'].append(service)
    result['jobs'].extend([
        {'name': names[0], 'condition': 'dipper.debug.start.hiview.events=true',
         'cmds': ['start hiview']},
        {'name': names[1], 'condition': 'dipper.debug.start.hiview.events=false',
         'cmds': ['stop hiview']},
    ])
    return result


def launcher_text(source, name):
    matches = re.findall(r'static const char ' + name + r'\[\] =\s*(.*?);', source, re.S)
    if len(matches) != 1:
        raise ValueError('Native HiView launcher text differs: ' + name)
    return ''.join(ast.literal_eval(token) for token in re.findall(r'"(?:\\.|[^"\\])*"', matches[0]))


# 由原定义生成确定性配置归档，保持事件校验和隐私处理顺序
def build_hiview_assets(inputs, source):
    validate_hiview_inputs(inputs)
    if digest(source) != HIVIEW_LAUNCHER_SOURCE:
        raise ValueError('Native HiView launcher source differs')
    text = source.decode('utf-8')
    files = {
        'config-tree/' + path.removeprefix('/system/etc/hiview/'): data
        for path, data in inputs.items() if path.startswith('/system/etc/hiview/')
    }
    files.update({
        'config-tree/adft_plugin_config': launcher_text(text, 'adft_text').encode(),
        'config-tree/bundle/plugin_bundle.json': launcher_text(text, 'bundle_text').encode(),
        'config-tree/bundle/plugin_bundle_ext.json': launcher_text(text, 'bundle_text').encode(),
        'config-tree/plugin_config': b'plugins:0\n',
    })
    output = io.BytesIO()
    with gzip.GzipFile(filename='', fileobj=output, mode='wb', mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as archive:
            for name, data in sorted(files.items()):
                entry = tarfile.TarInfo(name)
                entry.size, entry.mode, entry.mtime = len(data), 0o644, 0
                archive.addfile(entry, io.BytesIO(data))
    return output.getvalue(), files


# 只从已验证的原始包装源码构建静态入口
def build_hiview_launcher(source, tools, work):
    source = pathlib.Path(source)
    if digest(source.read_bytes()) != HIVIEW_LAUNCHER_SOURCE:
        raise ValueError('Native HiView launcher source differs')
    work.mkdir(parents=True, exist_ok=True)
    binary = work / 'hiview_namespace_launcher'
    command = [str(tools / 'sdk-6.0.0.47/native/llvm/bin/clang.exe'),
               '--target=aarch64-linux-ohos', '-O2', '-Wall', '-Wextra', '-Werror',
               '-std=c11', '-static', str(source), '-o', str(binary)]
    subprocess.run(command, check=True, capture_output=True)
    data = binary.read_bytes()
    if digest(data) != HIVIEW_LAUNCHER_BINARY:
        raise ValueError('Native HiView launcher build differs')
    return data


def build_hiview_helper(template, archive, files):
    values = {
        '@HIVIEW_LAUNCHER_SHA256@': HIVIEW_LAUNCHER_BINARY,
        '@HIVIEW_ASSETS_SHA256@': digest(archive),
        '@HIVIEW_ASSET_HASHES@': '\n'.join(
            digest(data) + '  ' + name for name, data in sorted(files.items())
        ),
    }
    for marker, value in values.items():
        if template.count(marker) != 1:
            raise ValueError('Native HiView helper marker differs: ' + marker)
        template = template.replace(marker, value)
    return template.encode('utf-8')


# 将构建输入和原生服务选择收束为独立可选载荷
def prepare_hiview_payload(desktop, inputs, repo, tools, work):
    source = repo / 'compat/services/hiview_namespace_launcher.c'
    selected = select_hiview_service(desktop, inputs)
    archive, files = build_hiview_assets(inputs, source.read_bytes())
    launcher = build_hiview_launcher(source, tools, work)
    helper = build_hiview_helper(
        (repo / 'runtime/compat/graphics/prepare-native-hiview-events.sh').read_text(
            encoding='utf-8'
        ),
        archive, files,
    )
    payload = {
        'hiview_namespace_launcher': (launcher, 0o100755),
        'hiview-events-assets.tar.gz': (archive, 0o100644),
        'prepare-native-hiview-events.sh': (helper, 0o100755),
    }
    report = {
        'enabled': True,
        'started_by_default': False,
        'startup_gate': 'dipper.debug.start.hiview.events',
        'request_preparation': 'prepare-native-hiview-events.sh <nonce>',
        'donor_inputs': HIVIEW_INPUTS,
        'launcher_source_sha256': HIVIEW_LAUNCHER_SOURCE,
        'launcher_sha256': digest(launcher),
        'assets_sha256': digest(archive),
        'selected_plugins': ['SysEventSource', 'SysEventStore', 'SysEventDispatcher',
                             'PrivacyController', 'EventValidator'],
        'donor_identity_and_schema_preserved': True,
        'private_persist_configuration_changed': False,
        'automatic_preparation': False,
        'activation_requires_manual_preparation': True,
        'sql_tracing_included': False,
        'runtime_validated': False,
    }
    return selected, payload, report

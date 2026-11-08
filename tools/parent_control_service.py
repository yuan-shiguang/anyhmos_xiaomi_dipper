import copy
import hashlib
import json
import re


PARENT_CONTROL_INPUTS = {
    '/system/etc/init/parentcontrolservice.cfg': '1ca7b84db9e752333a87bad56687b6c1086394ae80a6a6360f126798ecc8774e',
    '/system/profile/parent_control_service.json': 'bc05c799404357aa99f4de31e35d65fd73ef62956496f5e9b0def741a24d2a5e',
    '/system/lib64/libparentcontrol_service.z.so': 'e72abfd32f071ff9a88bded734cb7b19c8bc82267a69f09d57316257e80c4ced',
    '/system/etc/passwd': '4e70786ac3c6ace840da60e0608ef4424c9963298241c827a425c69705abc9b9',
    '/system/etc/group': '7c078f47d90b41127e6be365318e072c7639dc99a4073d5ab7a8d0f4aa68cb55',
}
PARENT_CONTROL_RUNTIME_SHA256 = '8793cf27b962d23aab2dc7497bfe0ad6341c0bc05a30b0c29a5b4f8c91e06ac4'
PARENT_CONTROL_JOBS = ('param:dipper-parentcontrol-start', 'param:dipper-parentcontrol-stop')
PARENT_CONTROL_RUNTIME_INPUTS = (
    '/system/profile/parent_control_service.json',
    '/system/lib64/libparentcontrol_service.z.so',
    '/system/etc/passwd',
    '/system/etc/group',
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


# 保留既有服务定义，拒绝重复导入和隐式启动入口
def validate_parent_control_init(config):
    if any('parentcontrol' in path.lower() or 'parent_control' in path.lower() or '*' in path
           for path in config.get('import', [])):
        raise ValueError('Parent control import already exists or needs review')
    if any(item['name'] == 'parent_control_service' for item in config.get('services', [])) or any(
        job['name'] in PARENT_CONTROL_JOBS or any(
            command.split()[:2] in (['start', 'parent_control_service'],
                                   ['stop', 'parent_control_service'])
            for command in job.get('cmds', [])
        )
        for job in config.get('jobs', [])
    ):
        raise ValueError('Parent control definition or startup already exists')


# 校验原生服务、按需策略及用户身份，不重放参数默认值
def validate_parent_control_inputs(inputs):
    if set(inputs) != set(PARENT_CONTROL_INPUTS):
        raise ValueError('Unexpected parent control donor input set')
    for name, expected in PARENT_CONTROL_INPUTS.items():
        if digest(inputs[name]) != expected:
            raise ValueError('Parent control donor hash mismatch: ' + name)
    users = [line.split(':') for line in inputs['/system/etc/passwd'].decode().splitlines()]
    selected = [line for line in users if line[0] == 'parent_control_service']
    groups = {
        line.split(':')[0]: line.split(':')[2]
        for line in inputs['/system/etc/group'].decode().splitlines()
        if line.split(':')[0] in ('parent_control_service', 'shell', 'oeminfo_nvme_server')
    }
    if len(selected) != 1 or selected[0][2:4] != ['7007', '7007'] or groups != {
        'parent_control_service': '7007', 'shell': '2000', 'oeminfo_nvme_server': '5004',
    }:
        raise ValueError('Parent control donor UID or GID mismatch')
    profile = json.loads(inputs['/system/profile/parent_control_service.json'])
    abilities = profile['systemability']
    if (
        profile['process'] != 'parent_control_service' or len(abilities) != 1
        or abilities[0]['name'] != 65862 or abilities[0]['run-on-create'] is not False
        or abilities[0]['auto-restart'] is not True
        or abilities[0]['start-on-demand']['bindercall'] is not True
    ):
        raise ValueError('Original parent control profile differs')


# 仅向备用桌面清单追加原生按需服务，保持原权限和策略
def select_parent_control_service(desktop, init_config, inputs):
    validate_parent_control_inputs(inputs)
    validate_parent_control_init(init_config)
    validate_parent_control_init(desktop)
    original = json.loads(inputs['/system/etc/init/parentcontrolservice.cfg'])
    if len(original['services']) != 1:
        raise ValueError('Unexpected parent control service count')
    service = copy.deepcopy(original['services'][0])
    if (
        service['name'] != 'parent_control_service' or service['uid'] != 'parent_control_service'
        or service['path'] != ['/system/bin/sa_main', '/system/profile/parent_control_service.json']
        or service['gid'] != ['parent_control_service', 'shell', 'oeminfo_nvme_server']
        or service['ondemand'] is not True or 'caps' in service
    ):
        raise ValueError('Original parent control service differs')
    service.update({'disabled': 1, 'once': 1, 'saspawn': False, 'start-mode': 'condition'})
    result = copy.deepcopy(desktop)
    result['services'].append(service)
    result['jobs'].extend([
        {'name': PARENT_CONTROL_JOBS[0], 'condition': 'dipper.debug.start.parentcontrol=true',
         'cmds': ['start parent_control_service']},
        {'name': PARENT_CONTROL_JOBS[1], 'condition': 'dipper.debug.start.parentcontrol=false',
         'cmds': ['stop parent_control_service']},
    ])
    return result


def prepare_parent_control_payload(desktop, init_config, inputs, repo):
    selected = select_parent_control_service(desktop, init_config, inputs)
    helper = (repo / 'runtime/compat/graphics/prepare-parentcontrol-el1.sh').read_bytes()
    candidate = (json.dumps(selected, indent=2) + '\n').encode()
    report = {
        'enabled': True,
        'sa': 65862,
        'ondemand': True,
        'native_profile_unchanged': True,
        'policy_defaults_replayed': False,
        'donor_identity_and_permissions_preserved': True,
        'existing_el2_required': '/data/service/el2/100/parentcontrol',
        'fresh_userdata_supported': False,
        'candidate_cfg_sha256': digest(candidate),
        'prepare_helper_sha256': digest(helper),
        'runtime_validated': False,
    }
    return candidate, helper, report


# 原目录和私有参数配置齐备后才原子选择备用清单
def add_parent_control_startup(runtime, baseline_cfg_sha, candidate_cfg_sha,
    helper_sha, private_cfg_sha):
    hashes = (baseline_cfg_sha, candidate_cfg_sha, helper_sha, private_cfg_sha)
    if (
        digest(runtime.encode()) != PARENT_CONTROL_RUNTIME_SHA256
        or any(not re.fullmatch('[0-9a-f]{64}', value) for value in hashes)
        or baseline_cfg_sha == candidate_cfg_sha or 'PARENTCONTROL_PROFILE_' in runtime
    ):
        raise ValueError('Parent control runtime prefix or hashes differ')
    checks = ''.join(
        f'    echo "{PARENT_CONTROL_INPUTS[path]}  $R{path}" | $B sha256sum -c - &&\n'
        for path in PARENT_CONTROL_RUNTIME_INPUTS
    )
    block = (
        '\n# 数据准备失败时保留原清单，不阻断核心服务\n'
        'parent_control_selected=0\n'
        'parent_control_cfg=$R/compat/etc/init-desktop-services.cfg\n'
        'parent_control_tmp=$R/compat/etc/.init-desktop-parentcontrol.tmp\n'
        'if $B test -d "$R/compat/etc" && $B test ! -L "$R/compat/etc" &&\n'
        '   [ "$($B readlink -f "$R/compat/etc")" = "$R/compat/etc" ] &&\n'
        '   [ "$($B stat -c \'%u:%g:%a\' "$R/compat/etc")" = 0:0:755 ] &&\n'
        '   $B test -f "$parent_control_cfg" && $B test ! -L "$parent_control_cfg" &&\n'
        '   [ "$($B stat -c \'%u:%g:%a:%h\' "$parent_control_cfg")" = 0:0:644:1 ] &&\n'
        '   [ "$($B readlink -f "$parent_control_cfg")" = "$parent_control_cfg" ] &&\n'
        f'   echo "{baseline_cfg_sha}  $parent_control_cfg" | $B sha256sum -c - &&\n'
        f'   echo "{private_cfg_sha}  /hmos-init-debug.cfg" | $B sha256sum -c - &&\n'
        + checks +
        f'    echo "{helper_sha}  /graphics/dipper/prepare-parentcontrol-el1.sh" '
        '| $B sha256sum -c - &&\n'
        f'    echo "{candidate_cfg_sha}  /graphics/dipper/init-desktop-parentcontrol.cfg" '
        '| $B sha256sum -c - &&\n'
        '    $B sh /graphics/dipper/prepare-parentcontrol-el1.sh &&\n'
        '    $B test ! -e "$parent_control_tmp" && $B test ! -L "$parent_control_tmp" &&\n'
        '    $B cp /graphics/dipper/init-desktop-parentcontrol.cfg "$parent_control_tmp" &&\n'
        '    $B chown 0:0 "$parent_control_tmp" && $B chmod 0644 "$parent_control_tmp" &&\n'
        '    $B test -f "$parent_control_tmp" && $B test ! -L "$parent_control_tmp" &&\n'
        '    [ "$($B stat -c \'%u:%g:%a:%h\' "$parent_control_tmp")" = 0:0:644:1 ] &&\n'
        '    [ "$($B readlink -f "$parent_control_tmp")" = "$parent_control_tmp" ] &&\n'
        f'    echo "{candidate_cfg_sha}  $parent_control_tmp" | $B sha256sum -c -; then\n'
        ' $B mv -T "$parent_control_tmp" "$parent_control_cfg" || :\n'
        ' if $B test -f "$parent_control_cfg" && $B test ! -L "$parent_control_cfg" &&\n'
        '    [ "$($B stat -c \'%u:%g:%a:%h\' "$parent_control_cfg")" = 0:0:644:1 ] &&\n'
        '    [ "$($B readlink -f "$parent_control_cfg")" = "$parent_control_cfg" ] &&\n'
        f'    echo "{candidate_cfg_sha}  $parent_control_cfg" | $B sha256sum -c -; then\n'
        '  parent_control_selected=1\n'
        ' fi\n'
        'fi\n'
        'if [ "$parent_control_selected" = 1 ]; then\n'
        ' echo PARENTCONTROL_PROFILE_SELECTED=65862\n'
        'elif $B test -f "$parent_control_cfg" && $B test ! -L "$parent_control_cfg" &&\n'
        '     [ "$($B stat -c \'%u:%g:%a:%h\' "$parent_control_cfg")" = 0:0:644:1 ] &&\n'
        '     [ "$($B readlink -f "$parent_control_cfg")" = "$parent_control_cfg" ] &&\n'
        f'     echo "{baseline_cfg_sha}  $parent_control_cfg" | $B sha256sum -c -; then\n'
        ' echo PARENTCONTROL_PROFILE_BASELINE_RETAINED=1\n'
        'else\n'
        ' echo PARENTCONTROL_PROFILE_UNKNOWN_SKIPPED >&2\n'
        'fi\n'
    )
    return runtime + block

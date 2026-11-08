import copy
import hashlib
import re


EXPERIENCE_RULE_INPUTS = {
    '/system/lib64/liberms.z.so': '103daede598f972c4f810c1295c493153558ec5d501cae0b858a70cefc9f7d40',
}
EXPERIENCE_RULE_PROFILE = {
    'name': 6105,
    'libpath': 'liberms.z.so',
    'run-on-create': True,
    'distributed': False,
    'dump_level': 1,
    'extension': ['backup', 'restore'],
    'min_hdi_proxy_version': [],
}


# 仅追加原生生态规则服务，保留已有清单及服务身份
def add_experience_rule_service(foundation, donor_profile, inputs):
    if set(inputs) != set(EXPERIENCE_RULE_INPUTS):
        raise ValueError('Unexpected experience rule input set')
    for path, expected in EXPERIENCE_RULE_INPUTS.items():
        if hashlib.sha256(inputs[path]).hexdigest() != expected:
            raise ValueError('Experience rule donor hash mismatch: ' + path)
    if (
        foundation['process'] != 'foundation'
        or donor_profile['process'] != 'foundation'
    ):
        raise ValueError('Unexpected experience rule process')
    abilities = foundation['systemability']
    identifiers = [item['name'] for item in abilities]
    if 6105 in identifiers or len(set(identifiers)) != len(identifiers):
        raise ValueError('Duplicate foundation system ability')
    selected = [item for item in donor_profile['systemability'] if item['name'] == 6105]
    if selected != [EXPERIENCE_RULE_PROFILE]:
        raise ValueError('Original experience rule profile differs')
    result = copy.deepcopy(foundation)
    result['systemability'].append(copy.deepcopy(selected[0]))
    return result


# 原子选择可选清单，输入或暂存失败时保留核心清单
def add_experience_rule_startup(startup, profile_sha256, core_sha256):
    anchor = '$B cp "$R/data/local/tmp/foundation-app-core.json" "$R/system/profile/foundation.json"\n'
    if (
        startup.count(anchor) != 1
        or startup.count('core_runtime_inputs_ready=1\n') != 1
        or 'ERMS_PROFILE_SELECTED=' in startup
        or not re.fullmatch('[0-9a-f]{64}', profile_sha256)
        or not re.fullmatch('[0-9a-f]{64}', core_sha256)
        or profile_sha256 == core_sha256
    ):
        raise ValueError('Experience rule startup anchors or profile hashes differ')
    library_path, library_sha256 = next(iter(EXPERIENCE_RULE_INPUTS.items()))
    block = (
        f' if echo "{library_sha256}  $R{library_path}" | $B sha256sum -c - &&\n'
        f'    echo "{profile_sha256}  /graphics/dipper/foundation-experience-rule.json" '
        '| $B sha256sum -c -; then\n'
        '  if $B test ! -e "$R/system/profile/.foundation-erms.tmp" &&\n'
        '     $B test ! -L "$R/system/profile/.foundation-erms.tmp" &&\n'
        '     $B cp /graphics/dipper/foundation-experience-rule.json '
        '"$R/system/profile/.foundation-erms.tmp" &&\n'
        '     $B chown 0:0 "$R/system/profile/.foundation-erms.tmp" &&\n'
        '     $B chmod 0644 "$R/system/profile/.foundation-erms.tmp" &&\n'
        f'     echo "{profile_sha256}  $R/system/profile/.foundation-erms.tmp" '
        '| $B sha256sum -c -; then\n'
        '   $B mv "$R/system/profile/.foundation-erms.tmp" '
        '"$R/system/profile/foundation.json" || :\n'
        '  fi\n'
        ' fi\n'
        f' if echo "{profile_sha256}  $R/system/profile/foundation.json" '
        '| $B sha256sum -c -; then\n'
        '  echo ERMS_PROFILE_SELECTED=6105\n'
        f' elif echo "{core_sha256}  $R/system/profile/foundation.json" '
        '| $B sha256sum -c -; then\n'
        '  echo ERMS_CORE_PROFILE_RETAINED=1\n'
        ' else\n'
        '  echo ERMS_PROFILE_STATE_UNKNOWN >&2\n'
        '  exit 68\n'
        ' fi\n'
    )
    return startup.replace(anchor, anchor + block, 1)

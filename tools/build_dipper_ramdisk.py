"""Apply reviewed GEM-W00 SP12 recipes to the known dipper vendor ramdisk.

Only host files are written. Proprietary input/output binaries stay outside Git.
Recipe evidence is static; this does not certify a successful device boot.
"""

import argparse
import copy
import difflib
import gzip
import hashlib
import io
import json
import pathlib
import re
import stat
import struct
import subprocess
import sys

CORE_RUNTIME_INPUTS = {
    '/system/etc/init/resource_schedule_service.cfg': '7403e1b03fc827ba20c428e72a65a2763eeae9da8178b728beacb01bd65e6cf1',
    '/system/profile/resource_schedule_service.json': '1333a417bd417b090495d2bd3a5299cd7c7c83334329b98b53358ee498a4b45b',
    '/system/etc/init/foundation.cfg': '5db6177ae0051696644dbc1af626aeceb5c5dba35e92642a98f5f2947e1dc579',
    '/system/profile/foundation.json': 'c69bab8ffaa1487359b85e316108840b870d218c9af94953eabf579443dd6544',
    '/system/lib64/libbgtaskmgr_service.z.so': 'b8267de19bfe14f683902eca399d9ab1db0940d35b2e14380cd3810ea288cbc6',
    '/system/lib64/libresschedsvc.z.so': '4bd4c90bc5ffff47ab2ffcecdcdd63b3afa15874f594099f454cc9915cc6bdc6',
    '/system/lib64/libans.z.so': 'c4ca859f4fe54e77bef552f575cc04f062c622c2e2f79e773a7e837a87a5e431',
    '/system/etc/passwd': '4e70786ac3c6ace840da60e0608ef4424c9963298241c827a425c69705abc9b9',
    '/system/etc/group': '7c078f47d90b41127e6be365318e072c7639dc99a4073d5ab7a8d0f4aa68cb55',
}

VIBRATOR_INPUTS = {
    '/system/etc/init/sensors_musl.cfg': '167368edbadb4eda0b82f14e7f9811edc53312fa029fed8f50ba06b414c0b8ce',
    '/system/profile/sensors.json': '13d9739b3bf7d1efd102af71e741599e436d458c2a5e8a11d33d934b5a4696f2',
    '/system/lib64/libmiscdevice_service.z.so': 'b74b4c79b4944e19b14baa6a520fb748749d5e0b27f413bb729de2c3e7b05666',
    '/system/lib64/libvibrator_proxy_2.0.z.so': '754dc014d11eb8af7ae549a73971038068827662c848fa3ee1282e4462374466',
    '/system/lib64/liblight_proxy_1.0.z.so': '2dd9278cfdf891706496416268cd4ec4802fb85cace8641aa1fb3743249bc90a',
    '/system/etc/passwd': '4e70786ac3c6ace840da60e0608ef4424c9963298241c827a425c69705abc9b9',
    '/system/etc/group': '7c078f47d90b41127e6be365318e072c7639dc99a4073d5ab7a8d0f4aa68cb55',
}

BOOTSTRAP_MEMBER = 'dipper-compat-v118/hmos-probe'
BOOTSTRAP_INPUT_SHA256 = '156f6ee5f33190ccb27581925b34c108fcb4df903aaf5b6868661248f0863013'
BOOTSTRAP_INPUT_BYTES = 687160
BOOTSTRAP_INPUT_MODE = 0o100755
BOOTSTRAP_IPC_PAIR = {
    'bootstrap_sha256': 'fb1545f6027ded68368b434dcb15e71b0003f2110a385d9ac2084df157e12407',
    'module_sha256': '34726f524452ad1251e99386d19315b847594bcd547f2d43c52329de97bddf67',
    'module_bytes': 554256,
    'module_input_sha256': '9ba7be77e3acd220889ef7ddda88c48e29e907f8f197c465e284c8cf3e749b0f',
    'module_input_bytes': 552952,
    'module_member': 'dipper-compat-v118/hmos-ipc.ko',
    'contract_sha256': 'bbc4e84ded7e702b06bffa2772735d96be202335da061a62c47b9265425b3ccf',
    'contract_bytes': 716800,
    'contract_member': 'dipper-compat-v118/graphics/dipper/binder_oneway_pid',
    'contract_alias': 'binder_oneway_pid',
}
BOOTSTRAP_CANDIDATES = {
    BOOTSTRAP_IPC_PAIR['bootstrap_sha256']: {
        'variant': 'ring-history-retained-oneway-pid',
        'source_sha256': '625c3a70e70b865e7806f4041e312e4171345106682e1e24a3c5057a937b6ba7',
        'serial_source_sha256': '3835dc47955a9024fccd64f38160e88d231eabaec02bac7ab42548ffe991b1d3',
    },
    '059218af4184b79ab42557acc9d8f73e607992078bbef754b7da8c1c66cd49cb': {
        'variant': 'ring-history-bounded-trace-drain',
        'source_sha256': 'a7e0df1cd61684b7a8973cede76706727f677c3c0ae2232b902a8467da75e4e8',
        'serial_source_sha256': '3835dc47955a9024fccd64f38160e88d231eabaec02bac7ab42548ffe991b1d3',
    },
    '203cc56a2c5cf66336bea9729796e03ecaccdc40b418c36fc717bc128ca1af39': {
        'variant': 'same-toolchain-source-baseline',
        'source_sha256': 'e62294311d04cfd0b45afc7ffd67d5222ec01ddff18dd454a3eed97324a4053f',
        'serial_source_sha256': '824d4dc8ad906d9af169a9f7af96c56c115c7c79a596d67fe1ccde4e351c9086',
    },
}

DISPLAY_LOG_TARGETS = (
    ('dipper-brightness-stderr.txt', '5528:5528', '0600'),
    ('dipper-composer-metrics.txt', '3036:3036', '0640'),
    ('dipper-battery-hdi.txt', '3025:3025', '0600'),
    ('dipper-wifi-category-v134.txt', '3026:3026', '0600'),
    ('dipper-bluetooth-hdi.txt', '3021:3021', '0600'),
    ('dipper-bluetooth-check.txt', '2000:2000', '0600'),
)
FILE_MIGRATE_HOOK = {
    'path': '/system/lib64/init/autorun/libfile_migrate_init.z.so',
    'input_sha256': '6c40082ea9222c7c0fad6e7af6f2d9ae3d46b347efa3d8fafbd1d8dd5c8ae45e',
    'output_sha256': 'bf86bb9c72095d537b92c1b1cf6d3bc45ba6c7a89e7522bae06b5c367d2c819a',
    'bytes': 66040,
    'uid': 0,
    'gid': 0,
    'mode': '0644',
}
RENDER_COMMIT_ASYNC = {
    'path': '/system/lib64/librender_service_base.z.so',
    'input_sha256': '18c5c79f335ec45e6fda4c5c8eb2e5175f1cfcb634c3f948fd6ed676db08ce22',
    'output_sha256': '429cf0c7eeb06ffb9e1b55a6a69cebe5356d42786fa337f0f33a8d8e2ed7ace9',
    'bytes': 10266192,
    'changes': [{'offset': 9245920, 'before': '01008052', 'after': '21008052'}],
    'guard': {
        'offset': 9245916,
        'before': 'e063009101008052c3320194',
        'after': 'e063009121008052c3320194',
    },
}

REPO = pathlib.Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--donor', type=pathlib.Path, required=True)
parser.add_argument('--original', type=pathlib.Path, required=True)
parser.add_argument('--tools', type=pathlib.Path, required=True)
parser.add_argument(
    '--audio-build-dir', type=pathlib.Path, required=True,
    help='Use the paired audio-vdi and pal-server outputs from build_current.py',
)
parser.add_argument('--work', type=pathlib.Path)
parser.add_argument('--composer', type=pathlib.Path)
parser.add_argument('--composer-sha256')
parser.add_argument('--allocator', type=pathlib.Path)
parser.add_argument('--allocator-sha256')
parser.add_argument('--present-fence-bridge', action='store_true')
parser.add_argument('--refresh-helper', type=pathlib.Path)
parser.add_argument('--refresh-helper-sha256')
parser.add_argument('--bootstrap', type=pathlib.Path)
parser.add_argument('--bootstrap-sha256')
parser.add_argument('--ipc-oneway-pid', action='store_true')
parser.add_argument('--ipc-module', type=pathlib.Path)
parser.add_argument('--ipc-module-sha256')
parser.add_argument('--ipc-contract', type=pathlib.Path)
parser.add_argument('--ipc-contract-sha256')
parser.add_argument('--render-commit-async', action='store_true')
parser.add_argument('--preserve-display-logs', action='store_true')
parser.add_argument(
    '--private-persist-parameters',
    action='store_true',
    help='Restore native private parameter loading after a checked cold-start mirror',
)
parser.add_argument(
    '--file-migrate-hook-guard',
    action='store_true',
    help='Guard the optional file-migrate argument in RAM; preserve native restorecon fallback',
)
parser.add_argument(
    '--vibrator-service',
    action='store_true',
    help='Restore native SA3602 startup; haptic hardware support depends on the device',
)
parser.add_argument(
    '--core-runtime-services',
    action='store_true',
    help='Restore native ANS3203 and RSS1901/1903 with original service declarations',
)
parser.add_argument(
    '--experience-rule-service',
    action='store_true',
    help='Experimentally add original ERMS6105 to the core foundation profile',
)
parser.add_argument(
    '--native-hiview-events',
    action='store_true',
    help='Package the default-disabled native five-plugin event service',
)
parser.add_argument(
    '--parent-control-service',
    action='store_true',
    help='Restore native SA65862 only after verified existing user data is prepared',
)
parser.add_argument(
    '--recipe-dir', type=pathlib.Path, default=REPO / 'patches/donor/gem-w00-111-sp12'
)
parser.add_argument(
    '--payload-out', type=pathlib.Path,
    help='同时把载荷子树导出成 cust 用的目录树（compat/...），供 build_dipper_images.py 打包',
)
parser.add_argument(
    '--regional-resources',
    dest='experimental_regional_resources',
    action=argparse.BooleanOptionalAction,
    default=True,
    help='Select verified GEM-W00 China resource layers; use --no-regional-resources for baseline comparison',
)
args = parser.parse_args()
if bool(args.composer) != bool(args.composer_sha256):
    parser.error('--composer and --composer-sha256 must be supplied together')
if bool(args.allocator) != bool(args.allocator_sha256):
    parser.error('--allocator and --allocator-sha256 must be supplied together')
if args.present_fence_bridge and not args.composer:
    parser.error('Present fence bridge requires a matching custom composer')
if bool(args.refresh_helper) != bool(args.refresh_helper_sha256) or (
    args.refresh_helper and not args.composer
):
    parser.error('Refresh helper requires its hash and a custom composer')
if bool(args.bootstrap) != bool(args.bootstrap_sha256):
    parser.error('--bootstrap and --bootstrap-sha256 must be supplied together')
ipc_inputs = (args.ipc_module, args.ipc_module_sha256, args.ipc_contract, args.ipc_contract_sha256)
if args.ipc_oneway_pid:
    if not all(ipc_inputs) or not args.bootstrap:
        parser.error('IPC pair requires a bootstrap, module, contract and all three hashes')
elif any(ipc_inputs):
    parser.error('IPC module and contract inputs require --ipc-oneway-pid')
if args.render_commit_async and not args.ipc_oneway_pid:
    parser.error('--render-commit-async requires --ipc-oneway-pid')
if args.experience_rule_service and not args.core_runtime_services:
    parser.error('--experience-rule-service requires --core-runtime-services')
if args.native_hiview_events and not args.core_runtime_services:
    parser.error('--native-hiview-events requires --core-runtime-services')
if args.parent_control_service and (
    not args.core_runtime_services or not args.private_persist_parameters
):
    parser.error('--parent-control-service requires core services and private parameters')
if (
    args.private_persist_parameters
    and args.bootstrap_sha256 != BOOTSTRAP_IPC_PAIR['bootstrap_sha256']
):
    parser.error('Private parameters require the verified userdata-ready bootstrap')
sys.path.insert(0, str(args.tools / 'python-libs'))
sys.path.insert(0, str(REPO / 'tools'))
from elftools.elf.elffile import ELFFile
import vendor_ramdisk_edit as vr
from build_dipper_images import cygpath
from private_persist_parameters import prepare_private_persist
from samgr_original_role import SAMGR_INPUTS, restore_samgr_role
from experience_rule_service import (
    EXPERIENCE_RULE_INPUTS,
    add_experience_rule_service,
    add_experience_rule_startup,
)
from hiview_event_service import (
    HIVIEW_INPUTS,
    prepare_hiview_payload,
    validate_hiview_init,
)
from parent_control_service import (
    PARENT_CONTROL_INPUTS,
    add_parent_control_startup,
    prepare_parent_control_payload,
)


# 核对静态 Linux 入口、装载区间及栈权限，不执行候选程序
def inspect_linux_bootstrap(blob):
    if not 64 <= len(blob) <= 16 * 1024 * 1024:
        raise ValueError('Bootstrap ELF size is outside the reviewed bounds')
    elf = ELFFile(io.BytesIO(blob))
    if (
        elf.elfclass != 64
        or not elf.little_endian
        or elf['e_ident']['EI_VERSION'] != 'EV_CURRENT'
        or elf['e_ident']['EI_OSABI'] not in ('ELFOSABI_SYSV', 'ELFOSABI_GNU', 'ELFOSABI_LINUX')
        or elf['e_machine'] != 'EM_AARCH64'
        or elf['e_type'] != 'ET_EXEC'
        or elf['e_ehsize'] != 64
        or elf['e_flags'] != 0
        or elf['e_version'] != 'EV_CURRENT'
    ):
        raise ValueError('Bootstrap must be a little-endian AArch64 executable')
    count, offset = elf['e_phnum'], elf['e_phoff']
    if not 1 <= count <= 128 or elf['e_phentsize'] != 56 or offset < 64:
        raise ValueError('Unexpected bootstrap program header layout')
    if offset + count * 56 > len(blob):
        raise ValueError('Bootstrap program headers extend beyond the input')
    if (
        not 1 <= elf['e_shnum'] <= 4096
        or elf['e_shentsize'] != 64
        or elf['e_shoff'] < 64
        or elf['e_shoff'] + elf['e_shnum'] * 64 > len(blob)
    ):
        raise ValueError('Bootstrap section headers extend beyond the input')
    loads, stacks, linux_notes = [], [], []
    for segment in elf.iter_segments():
        kind = segment['p_type']
        if kind in ('PT_INTERP', 'PT_DYNAMIC'):
            raise ValueError('Bootstrap must not require a dynamic loader')
        if segment['p_offset'] + segment['p_filesz'] > len(blob):
            raise ValueError('Bootstrap segment extends beyond the input')
        if kind == 'PT_LOAD':
            start, size = segment['p_vaddr'], segment['p_memsz']
            align = segment['p_align']
            if segment['p_filesz'] > size or start + size > 2**64:
                raise ValueError('Invalid bootstrap load size')
            if align > 1 and (
                align & (align - 1) or start % align != segment['p_offset'] % align
            ):
                raise ValueError('Invalid bootstrap load alignment')
            if segment['p_flags'] & 3 == 3:
                raise ValueError('Bootstrap must not contain writable executable loads')
            loads.append(
                {
                    'address': start,
                    'file_bytes': segment['p_filesz'],
                    'memory_bytes': size,
                    'flags': segment['p_flags'],
                }
            )
        elif kind == 'PT_GNU_STACK':
            stacks.append(segment['p_flags'])
        elif kind == 'PT_NOTE':
            for note in segment.iter_notes():
                if note['n_name'] == 'GNU' and note['n_type'] == 'NT_GNU_ABI_TAG':
                    desc = note['n_desc']
                    if desc['abi_os'] == 'ELF_NOTE_OS_LINUX':
                        linux_notes.append(
                            [desc['abi_major'], desc['abi_minor'], desc['abi_tiny']]
                        )
    if stacks != [6]:
        raise ValueError('Bootstrap requires exactly one non-executable read/write stack')
    entry = elf['e_entry']
    if entry % 4 or not any(
        item['flags'] & 1 and item['address'] <= entry < item['address'] + item['file_bytes']
        for item in loads
    ):
        raise ValueError('Bootstrap entry is outside executable file-backed loads')
    if not linux_notes:
        raise ValueError('Bootstrap lacks the reviewed GNU Linux ABI note')
    if any(section['sh_type'] == 'SHT_DYNAMIC' for section in elf.iter_sections()):
        raise ValueError('Bootstrap must not contain a dynamic section')
    return {
        'machine': elf['e_machine'],
        'type': elf['e_type'],
        'entry': entry,
        'loads': loads,
        'gnu_stack_flags': stacks,
        'gnu_linux_abi_minimum_kernel': linux_notes,
        'dynamic_loader_required': False,
    }


# 仅接受已独立核对 GNU 目标库和源码身份的 collector
def validate_bootstrap_binary(blob, expected_sha256, ipc_pair=False):
    if not re.fullmatch(r'[0-9a-f]{64}', expected_sha256):
        raise ValueError('Bootstrap hash must be a lowercase SHA256')
    digest = hashlib.sha256(blob).hexdigest()
    if digest != expected_sha256:
        raise ValueError('Bootstrap input hash mismatch')
    if digest not in BOOTSTRAP_CANDIDATES:
        raise ValueError('Bootstrap collector role and Linux runtime have not been reviewed')
    if digest == BOOTSTRAP_IPC_PAIR['bootstrap_sha256'] and not ipc_pair:
        raise ValueError('Retained-mode bootstrap requires the explicit IPC pair option')
    return {
        **BOOTSTRAP_CANDIDATES[digest],
        'sha256': digest,
        'elf': inspect_linux_bootstrap(blob),
        'role': 'linux-pid1-bootstrap-and-diagnostic-collector',
        'target_runtime': 'Ubuntu jammy AArch64 GNU/Linux glibc2.35 and GCC11.4 runtime archives',
        'runtime_evidence': 'Reviewed target archives and source identity; an ELF ABI note alone does not identify libc',
    }


# 锁定原归档成员和清单身份，仅改程序内容与文件大小
def replace_bootstrap(entries, blob, expected_sha256, ipc_pair=False):
    checked = validate_bootstrap_binary(blob, expected_sha256, ipc_pair)
    if BOOTSTRAP_MEMBER not in entries:
        raise ValueError('Original bootstrap member is missing')
    fields, previous = entries[BOOTSTRAP_MEMBER]
    if (
        len(fields) != 13
        or fields[1] != BOOTSTRAP_INPUT_MODE
        or fields[2:5] != [0, 0, 1]
        or fields[6] != BOOTSTRAP_INPUT_BYTES
        or len(previous) != BOOTSTRAP_INPUT_BYTES
        or hashlib.sha256(previous).hexdigest() != BOOTSTRAP_INPUT_SHA256
    ):
        raise ValueError('Original bootstrap content, mode or identity differs')
    manifest_name = BOOTSTRAP_MEMBER.rsplit('/', 1)[0] + '/MANIFEST.json'
    if manifest_name not in entries:
        raise ValueError('Original bootstrap manifest is missing')
    manifest = json.loads(entries[manifest_name][1])
    if manifest.get('hmos-probe') != {
        'bytes': BOOTSTRAP_INPUT_BYTES,
        'mode': oct(BOOTSTRAP_INPUT_MODE),
        'sha256': BOOTSTRAP_INPUT_SHA256,
    }:
        raise ValueError('Original bootstrap manifest identity differs')
    replacement = fields.copy()
    replacement[6] = len(blob)
    entries[BOOTSTRAP_MEMBER] = (replacement, blob)
    return {
        'enabled': True,
        'path': BOOTSTRAP_MEMBER,
        'input_sha256': BOOTSTRAP_INPUT_SHA256,
        'input_bytes': BOOTSTRAP_INPUT_BYTES,
        'output_sha256': checked.pop('sha256'),
        'output_bytes': len(blob),
        'mode': oct(fields[1]),
        'archive_metadata_preserved_except_size': True,
        **checked,
        'runtime_validated': False,
    }


# 固定入口、驱动与私有合约，拒绝混用两套运行方式
def validate_ipc_pair(bootstrap, module, contract, module_sha256, contract_sha256):
    expected = BOOTSTRAP_IPC_PAIR
    validate_bootstrap_binary(bootstrap, expected['bootstrap_sha256'], ipc_pair=True)
    for name, blob, supplied in (
        ('module', module, module_sha256), ('contract', contract, contract_sha256),
    ):
        if (
            supplied != expected[name + '_sha256']
            or hashlib.sha256(blob).hexdigest() != supplied
            or len(blob) != expected[name + '_bytes']
        ):
            raise ValueError('IPC ' + name + ' identity differs from the reviewed pair')
    elf = ELFFile(io.BytesIO(module))
    if elf['e_machine'] != 'EM_AARCH64' or elf['e_type'] != 'ET_REL':
        raise ValueError('IPC module must be an AArch64 relocatable ELF')
    return {
        'enabled': True,
        'bootstrap_sha256': expected['bootstrap_sha256'],
        'module_sha256': module_sha256,
        'contract_sha256': contract_sha256,
        'contract_elf': inspect_linux_bootstrap(contract),
        'oneway_sender_pid': True,
        'system_context_tested': False,
        'runtime_validated': False,
    }


# 保留驱动元数据，仅增加独立测试程序与根目录链接
def replace_ipc_pair(entries, module, contract):
    expected = BOOTSTRAP_IPC_PAIR
    if BOOTSTRAP_MEMBER not in entries:
        raise ValueError('Reviewed IPC bootstrap member is missing')
    checked = validate_ipc_pair(
        entries[BOOTSTRAP_MEMBER][1], module, contract,
        expected['module_sha256'], expected['contract_sha256'],
    )
    member = expected['module_member']
    manifest_name = BOOTSTRAP_MEMBER.rsplit('/', 1)[0] + '/MANIFEST.json'
    if member not in entries or manifest_name not in entries:
        raise ValueError('Original IPC module or manifest is missing')
    fields, previous = entries[member]
    if (
        len(fields) != 13 or fields[1] != 0o100600 or fields[2:5] != [0, 0, 1]
        or fields[6] != expected['module_input_bytes']
        or len(previous) != expected['module_input_bytes']
        or hashlib.sha256(previous).hexdigest() != expected['module_input_sha256']
    ):
        raise ValueError('Original IPC module content, mode or identity differs')
    manifest = json.loads(entries[manifest_name][1])
    if manifest.get('hmos-ipc.ko') != {
        'bytes': expected['module_input_bytes'], 'mode': '0o100600',
        'sha256': expected['module_input_sha256'],
    }:
        raise ValueError('Original IPC module manifest identity differs')
    program, alias = expected['contract_member'], expected['contract_alias']
    if program in entries or alias in entries:
        raise ValueError('Private IPC contract program or alias already exists')
    inode = max(value[0][0] for value in entries.values()) + 1
    additions = {}
    for name, blob, mode in (
        (program, contract, 0o100755),
        (alias, ('/' + program).encode(), 0o120777),
    ):
        additions[name] = (
            [inode, mode, 0, 0, 1, 0, len(blob), 0, 0, 0, 0, len(name.encode()) + 1, 0],
            blob,
        )
        inode += 1
    replacement = fields.copy()
    replacement[6] = len(module)
    entries[member] = (replacement, module)
    entries.update(additions)
    return {
        **checked,
        'module_path': member,
        'module_metadata_preserved_except_size': True,
        'contract_path': program,
        'contract_alias': alias,
        'contract_mode': '0o100755',
        'private_nodes': ['/dev/hmos_pidtest', '/dev/hmos_pidtest_ns'],
        'expected_retained_mode': 1,
        'builtin_contract_before_handoff_only': True,
        'contract_automatically_executed': False,
    }


# 固定已打补丁库的身份与指令上下文
def patch_render_commit_async(blob, recipe, pair):
    expected = RENDER_COMMIT_ASYNC
    if not pair or not pair.get('enabled') or any(
        pair.get(name + '_sha256') != BOOTSTRAP_IPC_PAIR[name + '_sha256']
        for name in ('bootstrap', 'module', 'contract')
    ):
        raise ValueError('Render async requires the reviewed retained-mode IPC pair')
    if recipe != expected:
        raise ValueError('Render async recipe differs from the reviewed single callsite')
    if len(blob) != expected['bytes'] or hashlib.sha256(blob).hexdigest() != expected['input_sha256']:
        raise ValueError('Render async requires the exact patched SYNC library')
    guard = expected['guard']
    offset = guard['offset']
    before = bytes.fromhex(guard['before'])
    if blob[offset:offset + len(before)] != before:
        raise ValueError('Render async instruction context differs')
    change = expected['changes'][0]
    at = change['offset']
    if at % 4 or blob[at:at + 4].hex() != change['before']:
        raise ValueError('Render async instruction differs')
    result = blob[:at] + bytes.fromhex(change['after']) + blob[at + 4:]
    after = bytes.fromhex(guard['after'])
    if (
        result[offset:offset + len(after)] != after
        or hashlib.sha256(result).hexdigest() != expected['output_sha256']
    ):
        raise ValueError('Render async output identity differs')
    return result, {
        'enabled': True,
        'path': expected['path'],
        'input_sha256': expected['input_sha256'],
        'output_sha256': expected['output_sha256'],
        'changed_instruction_offset': at,
        'changed_byte_offsets': [at],
        'original_donor_recipe_preserved': True,
        'other_vulkan_patches_preserved': True,
        'commit_async_restored': True,
        'runtime_validated': False,
    }


bootstrap_blob = None
if args.bootstrap:
    bootstrap_blob = args.bootstrap.read_bytes()
    validate_bootstrap_binary(bootstrap_blob, args.bootstrap_sha256, args.ipc_oneway_pid)
ipc_module_blob = ipc_contract_blob = None
ipc_input_report = None
if args.ipc_oneway_pid:
    ipc_module_blob = args.ipc_module.read_bytes()
    ipc_contract_blob = args.ipc_contract.read_bytes()
    ipc_input_report = validate_ipc_pair(
        bootstrap_blob, ipc_module_blob, ipc_contract_blob,
        args.ipc_module_sha256, args.ipc_contract_sha256,
    )

ROOT = args.donor
WORK = args.work or ROOT / 'port-work'
WORK.mkdir(parents=True, exist_ok=True)
P = 'dipper-compat-v118/'
G = P + 'graphics/'
base = args.original / 'images/vendor_boot.img'
expected_base = '6fd2ed9de8442a8c1301d8e8c3835a1bdf83ca145f25375491ba56e84b9fc615'
assert hashlib.sha256(base.read_bytes()).hexdigest() == expected_base
vr.MAGISKBOOT = str(args.tools / 'android-fs/magiskboot.exe')
container = base.read_bytes()
page = struct.unpack_from('<I', container, 12)[0]
header_size = struct.unpack_from('<I', container, 2096)[0]
ramdisk_size = struct.unpack_from('<I', container, 24)[0]
ramdisk_offset = (header_size + page - 1) // page * page
(WORK / 'base.lz4').write_bytes(container[ramdisk_offset : ramdisk_offset + ramdisk_size])
subprocess.run(
    [vr.MAGISKBOOT, 'decompress', str(WORK / 'base.lz4'), str(WORK / 'base.cpio')],
    check=True,
    capture_output=True,
)


# 仅提取索引中存在的供体普通文件
def extract_guard(path, previous=False):
    if not previous:
        part = path.split('/')[1]
        records = json.loads((ROOT / 'metadata' / f'{part}-files.json').read_text(encoding='utf-8'))
        relative = path if part == 'system' else '/' + path.split('/', 2)[2]
        record = next((x for x in records if x['path'] == relative), None)
        if record is None or record['type'] != 'FILE':
            raise ValueError('Guard input must be an indexed regular donor file: ' + path)
    destination = (
        ROOT
        / ('metadata/old-guard-inputs' if previous else 'metadata/guard-inputs')
        / path.lstrip('/')
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    partition = path.split('/')[1]
    image = (
        args.original / 'images/super.img'
        if previous
        else ROOT / 'extracted/base' / f'{partition}.img'
    )
    relative = path if partition == 'system' else '/' + path.split('/', 2)[2]
    command = (
        [str(args.tools / 'erofs-cygwin/dump.erofs.exe')]
        + (['--offset=996147200'] if previous else [])
        + ['--cat', '--path=' + relative, cygpath(image)]
    )
    result = subprocess.run(command, check=True, capture_output=True)
    destination.write_bytes(result.stdout)


recipe_paths = list(args.recipe_dir.glob('*.json'))
if args.present_fence_bridge:
    recipe_paths.append(REPO / 'patches/display/present-fence-export.json')
if args.file_migrate_hook_guard:
    recipe_paths.append(REPO / 'patches/init/file-migrate-hook-guard.json')
for recipe_path in recipe_paths:
    extract_guard(json.loads(recipe_path.read_text(encoding='utf-8'))['path'])
for path in [
    '/system/lib64/libaccountmgr.z.so',
    '/system/lib64/chipset-sdk/libhukssdk.z.so',
    '/system/lib64/chipset-sdk-sp/libcrypto_openssl.z.so',
    '/sys_prod/etc/param/hw_defaults.para',
    '/system/etc/sandbox/appdata-sandbox.json',
    '/system/etc/sandbox/appdata-sandbox-isolated.json',
    '/system/etc/init/task_manager_service.cfg',
    '/system/profile/task_manager_service.json',
    '/system/etc/init/huawei_id_svc.cfg',
    '/system/profile/huawei_id_svc.json',
    '/system/lib64/libhuaweiid_svc.z.so',
    '/system/etc/passwd',
    '/system/etc/group',
]:
    extract_guard(path)
for path in SAMGR_INPUTS:
    extract_guard(path)
if args.core_runtime_services:
    for path in CORE_RUNTIME_INPUTS:
        extract_guard(path)
if args.experience_rule_service:
    for path in EXPERIENCE_RULE_INPUTS:
        extract_guard(path)
if args.vibrator_service:
    for path in VIBRATOR_INPUTS:
        extract_guard(path)
if args.native_hiview_events:
    for path in HIVIEW_INPUTS:
        extract_guard(path)
if args.parent_control_service:
    for path in PARENT_CONTROL_INPUTS:
        extract_guard(path)
for name in ['appdata-sandbox.json', 'appdata-sandbox-isolated.json']:
    extract_guard('/system/etc/sandbox/' + name, previous=True)
old = vr.parse((WORK / 'base.cpio').read_bytes())
new = dict(old)
bootstrap_report = {'enabled': False}
if bootstrap_blob is not None:
    bootstrap_report = replace_bootstrap(
        new, bootstrap_blob, args.bootstrap_sha256, args.ipc_oneway_pid
    )
ipc_report = {'enabled': False}


# 复制文件元数据，避免改动原始归档
def setfile(name, data, mode=0o100644):
    if isinstance(data, str):
        data = data.encode()
    if name in new:
        fields = new[name][0].copy()
    else:
        fields = [
            max(v[0][0] for v in new.values()) + 1,
            mode,
            0,
            0,
            1,
            0,
            len(data),
            0,
            0,
            0,
            0,
            len(name.encode()) + 1,
            0,
        ]
    fields[1] = mode
    fields[6] = len(data)
    new[name] = (fields, data)


def text(name):
    return old[G + name][1].decode()


def sha(b):
    return hashlib.sha256(b).hexdigest()


# 成对收纳音频构建产物，并同步原音频清单
def replace_audio_payload(entries, build_dir):
    prefix = 'dipper-audio-v1/'
    inputs = {
        'audio/native/lib64/libdipper_audio_vdi.so': 'libdipper_audio_vdi.so',
        'audio-pal-v1/libdipper_pal_server_v1c.so': 'libdipper_pal_server.so',
    }
    payloads = {
        relative: (build_dir / filename).read_bytes()
        for relative, filename in inputs.items()
    }
    manifest = json.loads(entries[prefix + 'manifest.json'][1])
    replacements = {}
    result = {}
    for relative, blob in payloads.items():
        digest = sha(blob)
        replacements[prefix + relative] = blob
        manifest['files'][relative].update(bytes=len(blob), sha256=digest)
        result[relative] = {
            'source': str(build_dir / inputs[relative]),
            'bytes': len(blob),
            'sha256': digest,
        }
    # sdm845 没有 AGM，tinyalsa 后端随音频载荷一起发布。
    # 构建目录里没有它时保持载荷原样，start-audio-v116.sh 会明确报
    # AUDIO_BACKEND_ABSENT_NO_TINYALSA，不会静默降级。
    backend_relative = 'audio/native/bin/dipper-tinyalsa-server'
    backend = build_dir / 'dipper-tinyalsa-server'
    if backend.is_file():
        blob = backend.read_bytes()
        digest = sha(blob)
        replacements[prefix + backend_relative] = blob
        template = manifest['files'][next(iter(inputs))]
        manifest['files'][backend_relative] = {
            key: value for key, value in template.items()
            if key not in ('bytes', 'sha256')
        }
        manifest['files'][backend_relative].update(bytes=len(blob), sha256=digest)
        if 'mode' in manifest['files'][backend_relative]:
            manifest['files'][backend_relative]['mode'] = '0755'
        result[backend_relative] = {
            'source': str(backend),
            'bytes': len(blob),
            'sha256': digest,
            'mode': '0755',
        }
        name = prefix + backend_relative
        entries[name] = ([
            max(v[0][0] for v in entries.values()) + 1,
            0o100755, 0, 0, 1, 0, len(blob), 0, 0, 0, 0,
            len(name.encode()) + 1, 0,
        ], blob)
    else:
        result['dipper-tinyalsa-server'] = {'present': False}
    sums = []
    listed = set()
    for line in entries[prefix + 'SHA256SUMS'][1].decode().splitlines():
        digest, relative = line.split('  ', 1)
        if relative in result and 'sha256' in result[relative]:
            digest = result[relative]['sha256']
        sums.append(digest + '  ' + relative)
        listed.add(relative)
    if backend_relative in result and backend_relative not in listed:
        sums.append(result[backend_relative]['sha256'] + '  ' + backend_relative)
    replacements[prefix + 'SHA256SUMS'] = ('\n'.join(sums) + '\n').encode()
    replacements[prefix + 'manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    for name, blob in replacements.items():
        fields = entries[name][0].copy()
        fields[6] = len(blob)
        entries[name] = (fields, blob)
    return result


# 仅在显式选项下校验迁移钩子身份
def validate_file_migrate_hook(recipe, records, blob):
    expected = FILE_MIGRATE_HOOK
    if any(recipe.get(key) != expected[key] for key in ('path', 'input_sha256', 'output_sha256')):
        raise ValueError('File-migrate hook recipe identity differs')
    matches = [record for record in records if record.get('path') == expected['path']]
    if len(matches) != 1:
        raise ValueError('File-migrate hook donor record is missing or duplicated')
    record = matches[0]
    if record.get('type') != 'FILE' or any(
        record.get(key) != expected[key] for key in ('uid', 'gid', 'mode')
    ):
        raise ValueError('File-migrate hook donor metadata differs')
    if len(blob) != expected['bytes'] or sha(blob) != expected['input_sha256']:
        raise ValueError('File-migrate hook donor content differs')
    return {
        'enabled': True,
        **expected,
        'target': '/compat/lib64/libfile_migrate_init.z.so',
        'installation': 'RAM bind at original autorun path before native init',
        'null_optional_argument': 'return -1 to original restorecon fallback',
        'first_path_must_be_nonnull': True,
        'runtime_validated': False,
    }


# 校验库身份后覆盖自动加载路径，仅作用于内存挂载
def add_file_migrate_hook_guard(compat):
    marker = 'FILE_MIGRATE_HOOK_GUARD_STAGED'
    if compat.count('B=/busybox\nR=/hmos-root\n') != 1 or marker in compat:
        raise ValueError('File-migrate hook staging anchor differs')
    if FILE_MIGRATE_HOOK['path'] in compat:
        raise ValueError('File-migrate hook path is already staged')
    return compat + (
        f'file_migrate_source="$R{FILE_MIGRATE_HOOK["path"]}"\n'
        'file_migrate_target="$R/compat/lib64/libfile_migrate_init.z.so"\n'
        '$B test -f "$file_migrate_source"\n'
        '$B test ! -L "$file_migrate_source"\n'
        '[ "$($B stat -c %u:%g:%a "$file_migrate_source")" = 0:0:644 ]\n'
        f'echo "{FILE_MIGRATE_HOOK["input_sha256"]}  $file_migrate_source" | $B sha256sum -c -\n'
        '$B test ! -e "$file_migrate_target"\n'
        '$B test ! -L "$file_migrate_target"\n'
        '$B cp /graphics/dipper/libfile_migrate_init.z.so "$file_migrate_target"\n'
        '$B chown 0:0 "$file_migrate_target"\n'
        '$B chmod 0644 "$file_migrate_target"\n'
        f'echo "{FILE_MIGRATE_HOOK["output_sha256"]}  $file_migrate_target" | $B sha256sum -c -\n'
        '[ "$($B stat -c %u:%g:%a "$file_migrate_target")" = 0:0:644 ]\n'
        '$B mount --bind "$file_migrate_target" "$file_migrate_source"\n'
        '$B mount -o remount,bind,ro "$file_migrate_source"\n'
        f'echo "{FILE_MIGRATE_HOOK["output_sha256"]}  $file_migrate_source" | $B sha256sum -c -\n'
        '[ "$($B stat -c %u:%g:%a "$file_migrate_source")" = 0:0:644 ]\n'
        f'echo {marker}\n'
    )


# 仅按显式选项复用源码中的日志保留逻辑
def enable_display_log_retention(hardware, tracked):
    begin = '# DIPPER_DISPLAY_LOG_RETENTION_BEGIN\n'
    end = '# DIPPER_DISPLAY_LOG_RETENTION_END\n'
    anchor = 'B=/busybox\nR=/hmos-root\n'
    if (
        tracked.count(begin) != 1
        or tracked.count(end) != 1
        or hardware.count(anchor) != 1
        or 'DIPPER_DISPLAY_LOG_RETENTION' in hardware
    ):
        raise ValueError('Display log retention anchors differ')
    start = tracked.index(begin)
    stop = tracked.index(end) + len(end)
    if start >= stop or tracked[start:stop].count('PRESERVE_DISPLAY_LOGS=0\n') != 1:
        raise ValueError('Display log retention default differs')
    block = tracked[start:stop].replace(
        'PRESERVE_DISPLAY_LOGS=0\n', 'PRESERVE_DISPLAY_LOGS=1\n', 1
    )
    output = hardware
    for name, owner, mode in DISPLAY_LOG_TARGETS:
        path = '$R/data/local/tmp/' + name
        original = f': > {path}\n$B chown {owner} {path}\n$B chmod {mode} {path}\n'
        replacement = f'stage_hardware_log "{path}" {owner} {mode}\n'
        if output.count(original) != 1 or tracked.count(replacement) != 1:
            raise ValueError('Display log staging identity differs: ' + name)
        output = output.replace(original, replacement, 1)
    return output.replace(anchor, anchor + block, 1)


# 沿用原隐私服务声明，在访问令牌服务之后启动
def add_privacy_service(platform, native_init, tracked_platform):
    selected = [s for s in tracked_platform['services'] if s['name'] == 'privacy_service']
    if len(selected) != 1:
        raise ValueError('Unexpected privacy service declaration')
    service = selected[0]
    existing = [s for s in platform['services'] if s['name'] == 'privacy_service']
    if existing and existing != selected:
        raise ValueError('Privacy service declaration differs')
    if not existing:
        platform['services'].append(copy.deepcopy(service))
    coredeps = next(j for j in native_init['jobs'] if j['name'] == 'param:dipper-debug-coredeps')
    commands = coredeps['cmds']
    position = commands.index('start accesstoken_service') + 1
    if 'start privacy_service' not in commands:
        commands.insert(position, 'start privacy_service')
    elif commands[position:position + 1] != ['start privacy_service']:
        raise ValueError('Privacy service startup order differs')


# 保留原厂账户服务身份、权限及目录准备任务
def add_huawei_id_service(desktop, donor):
    if len(donor['services']) != 1:
        raise ValueError('Unexpected HuaweiID service count')
    service = donor['services'][0].copy()
    if service['name'] != 'huawei_id_svc' or service['uid'] != 'huawei_id':
        raise ValueError('Unexpected HuaweiID service identity')
    if any(item['name'] == service['name'] for item in desktop['services']):
        raise ValueError('HuaweiID service is already defined')
    job_names = {item['name'] for item in desktop['jobs']}
    gate_name = 'param:dipper-huaweiid-service'
    if job_names & ({item['name'] for item in donor['jobs']} | {gate_name}):
        raise ValueError('HuaweiID startup job is already defined')
    service.update({'disabled': 1, 'once': 1, 'saspawn': False, 'start-mode': 'condition'})
    desktop['services'].append(service)
    desktop['jobs'].extend(donor['jobs'])
    desktop['jobs'].append(
        {
            'name': gate_name,
            'condition': 'dipper.debug.start.huaweiid=true',
            'cmds': ['start huawei_id_svc'],
        }
    )


# 在完整包管理启动后请求原厂账户服务，失败时保留日志并继续启动
def add_huawei_id_startup(startup):
    anchor = 'stage=scene\ntrace_stage\ndeadline=$(($(now)+600))\n'
    if startup.count(anchor) != 1 or 'stage=huaweiid\n' in startup:
        raise ValueError('HuaweiID startup insertion point is absent or ambiguous')
    block = '''stage=huaweiid
trace_stage
huaweiid_ready=1
deadline=$(($(now)+20))
for id in 200 401 1151 3299; do
 while ! $B timeout 3 $B chroot "$R" /compat/bin/native_sa_check "$id"; do
  if [ "$(now)" -ge "$deadline" ]; then
   echo "HUAWEIID_DEPENDENCY_NOT_READY=$id"
   huaweiid_ready=0
   break
  fi
  $B sleep 1
 done
done
if [ "$huaweiid_ready" -eq 1 ]; then
 if $B chroot "$R" /system/bin/param set dipper.debug.start.huaweiid true; then
  deadline=$(($(now)+15))
  while :; do
   if $B timeout 3 $B chroot "$R" /compat/bin/native_sa_check 65926; then
    echo HUAWEIID_SA65926_READY=1
    break
   fi
   if [ "$(now)" -ge "$deadline" ]; then
    echo HUAWEIID_SA65926_NOT_READY=1
    break
   fi
   $B sleep 1
  done
 else
  echo HUAWEIID_START_REQUEST_FAILED=1
 fi
fi
'''
    return startup.replace(anchor, block + anchor, 1)


# 核对核心服务供体摘要及原生用户、组编号
def validate_core_runtime_inputs(inputs):
    if set(inputs) != set(CORE_RUNTIME_INPUTS):
        raise ValueError('Unexpected core runtime input set')
    for path, expected in CORE_RUNTIME_INPUTS.items():
        if sha(inputs[path]) != expected:
            raise ValueError('Core runtime donor hash mismatch: ' + path)
    users = {
        fields[0]: fields[2:4]
        for line in inputs['/system/etc/passwd'].decode().splitlines()
        if len(fields := line.split(':')) >= 4
    }
    groups = {
        fields[0]: fields[2]
        for line in inputs['/system/etc/group'].decode().splitlines()
        if len(fields := line.split(':')) >= 3
    }
    if any(
        users.get(name) != pair
        for name, pair in {
            'ressched': ['1096', '1096'],
            'foundation': ['5523', '5523'],
        }.items()
    ) or any(
        groups.get(name) != value
        for name, value in {
            'ressched': '1096',
            'system': '1000',
            'battery_extra_host': '5001',
            'foundation': '5523',
        }.items()
    ):
        raise ValueError('Core runtime native UID or GID mismatch')


# 仅追加通知与两项资源调度服务，保留原生身份和既有配置
def add_core_runtime_services(desktop, foundation, foundation_service, donor):
    if (
        foundation['process'] != 'foundation'
        or donor['foundation_profile']['process'] != 'foundation'
    ):
        raise ValueError('Unexpected foundation process')
    original = next(
        s for s in donor['foundation_cfg']['services'] if s['name'] == 'foundation'
    )
    identity = [
        'uid',
        'gid',
        'caps',
        'apl',
        'permission',
        'permission_acls',
        'kernel_permission',
        'secon',
    ]
    if foundation_service.get('uid') != 'foundation' or any(
        foundation_service.get(key) != original.get(key) for key in identity
    ):
        raise ValueError('Foundation native identity or permissions differ')
    service_name = 'resource_schedule_service'
    gate_name = 'param:dipper-core-runtime-services'
    if any(s['name'] == service_name for s in desktop['services']) or any(
        j['name'] == gate_name for j in desktop['jobs']
    ):
        raise ValueError('Core runtime service or gate is already defined')
    matches = [
        s for s in donor['resource_cfg']['services'] if s['name'] == service_name
    ]
    if len(matches) != 1 or donor['resource_profile']['process'] != service_name:
        raise ValueError('Unexpected native resource service')
    service = copy.deepcopy(matches[0])
    if (
        service['uid'] != 'ressched'
        or service['gid'] != ['ressched', 'system', 'battery_extra_host']
        or service['caps'] != ['SYS_NICE']
        or len(service['permission']) != 67
        or len(service['permission_acls']) != 7
    ):
        raise ValueError('Unexpected resource service identity or permissions')
    ans = [
        sa for sa in donor['foundation_profile']['systemability'] if sa['name'] == 3203
    ]
    if (
        len(ans) != 1
        or ans[0]['libpath'] != 'libans.z.so'
        or ans[0].get('run-on-create') is not True
        or ans[0].get('depend') != [3299]
        or any(sa['name'] == 3203 for sa in foundation['systemability'])
    ):
        raise ValueError('Unexpected or duplicate native ANS ability')
    # 通知服务依赖唯一且随进程启动的公共事件服务
    ces = [sa for sa in foundation['systemability'] if sa['name'] == 3299]
    if (
        len(ces) != 1
        or ces[0].get('libpath') != 'libcesfwk_services.z.so'
        or ces[0].get('run-on-create') is not True
    ):
        raise ValueError('Unexpected or duplicate native common event ability')
    selected = []
    for ident, libpath in [
        (1901, 'libresschedsvc.z.so'),
        (1903, 'libbgtaskmgr_service.z.so'),
    ]:
        abilities = [
            sa
            for sa in donor['resource_profile']['systemability']
            if sa['name'] == ident
        ]
        if len(abilities) != 1 or abilities[0]['libpath'] != libpath:
            raise ValueError('Unexpected core resource ability')
        selected.append(copy.deepcopy(abilities[0]))
    directories = []
    postfs = [j for j in donor['resource_cfg']['jobs'] if j['name'] == 'post-fs-data']
    if len(postfs) != 1:
        raise ValueError('Unexpected resource directory job')
    for name, mode in [
        ('ressched', '0700'),
        ('efficiency_manager', '0700'),
        ('device_standby', '0700'),
        ('appnap', '0700'),
        ('background_task_mgr', '0711'),
        ('WorkScheduler', '0711'),
        ('bundle_usage', '0711'),
    ]:
        command = f'mkdir /data/service/el1/public/{name} {mode} ressched ressched'
        if postfs[0]['cmds'].count(command) != 1:
            raise ValueError('Native resource directory contract differs: ' + name)
        directories.append(command)
    service.update(
        {
            'path': ['/system/bin/sa_main', '/compat/etc/resource-schedule-core.json'],
            'disabled': 1,
            'once': 1,
            'saspawn': False,
            'start-mode': 'condition',
        }
    )
    result = copy.deepcopy(foundation)
    result['systemability'].append(copy.deepcopy(ans[0]))
    resource = copy.deepcopy(donor['resource_profile'])
    resource['systemability'] = selected
    desktop['services'].append(service)
    desktop['jobs'].append(
        {
            'name': gate_name,
            'condition': 'dipper.debug.start.resourceschedule=true',
            'cmds': directories + ['start resource_schedule_service'],
        }
    )
    return result, resource


# 原配置声明服务依赖，启动时不重复校验镜像内文件

def add_core_runtime_startup(startup):
    anchor = 'stage=scene\ntrace_stage\ndeadline=$(($(now)+600))\n'
    if startup.count(anchor) != 1 or 'stage=core_runtime\n' in startup:
        raise ValueError('Core runtime startup point is missing or ambiguous')
    block = """stage=core_runtime
trace_stage
if ! $B chroot "$R" /system/bin/param set dipper.debug.start.resourceschedule true; then
 echo CORE_RUNTIME_START_REQUEST_FAILED=1
fi
"""
    return startup.replace(anchor, block + anchor, 1)



# 核对振动服务供体摘要及原生用户、组编号
def validate_vibrator_inputs(inputs):
    if set(inputs) != set(VIBRATOR_INPUTS):
        raise ValueError('Unexpected vibrator input set')
    for path, expected in VIBRATOR_INPUTS.items():
        if sha(inputs[path]) != expected:
            raise ValueError('Vibrator donor hash mismatch: ' + path)
    for filename, expected in [
        ('passwd', {'sensor': ['6688', '6688'], 'shell': ['2000', '2000']}),
        ('group', {'sensor': ['6688'], 'shell': ['2000']}),
    ]:
        rows = [
            line.split(':')
            for line in inputs['/system/etc/' + filename].decode().splitlines()
        ]
        for name, numbers in expected.items():
            found = [row for row in rows if row[0] == name]
            if len(found) != 1 or found[0][2 : 2 + len(numbers)] != numbers:
                raise ValueError('Vibrator native UID or GID mismatch: ' + name)


# 仅选择原生振动 SA，保留身份权限和原数据目录
def add_vibrator_service(desktop, cfg, profile):
    gates = ('param:dipper-vibrator-service', 'param:dipper-vibrator-service-stop')
    if any(s['name'] == 'sensors' for s in desktop['services']) or any(
        j['name'] in gates
        or j.get('condition')
        in (
            'dipper.debug.start.vibrator=true',
            'dipper.debug.start.vibrator=false',
        )
        for j in desktop['jobs']
    ):
        raise ValueError('Vibrator service or gate is already defined')
    services = [s for s in cfg['services'] if s['name'] == 'sensors']
    abilities = [sa for sa in profile['systemability'] if sa['name'] == 3602]
    if (
        len(services) != 1
        or profile['process'] != 'sensors'
        or len(abilities) != 1
        or abilities[0]['libpath'] != 'libmiscdevice_service.z.so'
        or abilities[0].get('run-on-create') is not True
        or abilities[0].get('min_hdi_proxy_version') != ['libvibrator_proxy_2.0.z.so']
    ):
        raise ValueError('Unexpected native vibrator service or ability')
    service = copy.deepcopy(services[0])
    if (
        service['path'] != ['/system/bin/sa_main', '/system/profile/sensors.json']
        or service['uid'] != 'sensor'
        or service['gid'] != ['sensor', 'shell']
        or service['secon'] != 'u:r:sensors:s0'
        or len(service['permission']) != 9
        or service['permission_acls'] != ['ohos.permission.GET_SENSITIVE_PERMISSIONS']
    ):
        raise ValueError('Unexpected vibrator identity or permissions')
    boot = [job for job in cfg['jobs'] if job['name'] == 'boot']
    directories = [
        'mkdir /data/service/el1/public/sensor',
        'chown sensor sensor /data/service/el1/public/sensor',
    ]
    if len(boot) != 1 or boot[0]['cmds'] != directories:
        raise ValueError('Unexpected native sensor directory job')
    service.update(
        {
            'path': ['/system/bin/sa_main', '/compat/etc/sensors-vibrator-only.json'],
            'disabled': 1,
            'once': 1,
            'saspawn': False,
            'start-mode': 'condition',
        }
    )
    selected = copy.deepcopy(profile)
    selected['systemability'] = copy.deepcopy(abilities)
    desktop['services'].append(service)
    desktop['jobs'].extend(
        [
            {
                'name': gates[0],
                'condition': 'dipper.debug.start.vibrator=true',
                'cmds': directories + ['start sensors'],
            },
            {
                'name': gates[1],
                'condition': 'dipper.debug.start.vibrator=false',
                'cmds': ['stop sensors'],
            },
        ]
    )
    return selected


# 后台暂存原服务清单并请求启动，不轮询诊断SA

def add_vibrator_startup(startup):
    anchor = 'stage=scene\ntrace_stage\ndeadline=$(($(now)+600))\n'
    if startup.count(anchor) != 1 or 'VIBRATOR_START_REQUEST_FAILED=' in startup:
        raise ValueError('Vibrator startup point is missing or ambiguous')
    block = """(
 if $B mkdir -p "$R/compat/etc" &&
    $B cp /graphics/dipper/sensors-vibrator-only.json "$R/compat/etc/sensors-vibrator-only.json" &&
    $B chmod 0644 "$R/compat/etc/sensors-vibrator-only.json"; then
  if ! $B chroot "$R" /system/bin/param set dipper.debug.start.vibrator true; then
   echo VIBRATOR_START_REQUEST_FAILED=1
  fi
 else
  echo VIBRATOR_PROFILE_STAGE_FAILED=1
 fi
) &
"""
    return startup.replace(anchor, block + anchor, 1)



# 核对新显示库的架构、导出及平台 C++ ABI
composer_report = None
if args.composer:
    blob = args.composer.read_bytes()
    if sha(blob) != args.composer_sha256:
        raise ValueError('Composer input hash mismatch')
    # 真实呈现反馈必须配套输出描述符清理补丁
    if (b'PRESENT_FENCE first_fd=' in blob) != args.present_fence_bridge:
        raise ValueError('Composer and present fence bridge must be paired')
    elf = ELFFile(io.BytesIO(blob))
    if elf['e_machine'] != 'EM_AARCH64' or elf['e_type'] != 'ET_DYN':
        raise ValueError('Composer must be an AArch64 shared library')
    symbols = list(elf.get_section_by_name('.dynsym').iter_symbols())
    exports = {s.name for s in symbols if s['st_shndx'] != 'SHN_UNDEF'}
    header = (REPO / 'compat/display/include/vdi-only/composer_exports.h').read_text(
        encoding='utf-8'
    )
    required = set(re.findall(r'extern "C" int32_t (\w+)\(', header))
    if not required <= exports or any('__n1' in s.name for s in symbols):
        raise ValueError('Composer VDI exports or platform C++ ABI mismatch')
    needed = {
        t.needed
        for t in elf.get_section_by_name('.dynamic').iter_tags()
        if t.entry.d_tag == 'DT_NEEDED'
    }
    if not needed <= {'libc++.so', 'libc.so'}:
        raise ValueError('Unexpected composer runtime dependency')
    member = G + 'libdisplay_composer_vdi_impl.z.so'
    composer_report = {
        'input_sha256': sha(old[member][1]),
        'output_sha256': sha(blob),
        'required_exports_checked': len(required),
        'needed': sorted(needed),
    }
    setfile(member, blob)


# 核对缓冲区实现的架构、工厂导出和平台 C++ ABI
allocator_report = None
if args.allocator:
    blob = args.allocator.read_bytes()
    if sha(blob) != args.allocator_sha256:
        raise ValueError('Allocator input hash mismatch')
    elf = ELFFile(io.BytesIO(blob))
    if elf['e_machine'] != 'EM_AARCH64' or elf['e_type'] != 'ET_DYN':
        raise ValueError('Allocator must be an AArch64 shared library')
    symbols = list(elf.get_section_by_name('.dynsym').iter_symbols())
    exports = {s.name for s in symbols if s['st_shndx'] != 'SHN_UNDEF'}
    required = {'CreateDisplayBufferVdi', 'DestroyDisplayBufferVdi'}
    if not required <= exports or any('__n1' in s.name for s in symbols):
        raise ValueError('Allocator VDI exports or platform C++ ABI mismatch')
    needed = {
        t.needed
        for t in elf.get_section_by_name('.dynamic').iter_tags()
        if t.entry.d_tag == 'DT_NEEDED'
    }
    if not needed <= {'libc++.so', 'libc.so'}:
        raise ValueError('Unexpected allocator runtime dependency')
    member = G + 'libdisplay_buffer_vdi_impl.z.so'
    allocator_report = {
        'input_sha256': sha(old[member][1]),
        'output_sha256': sha(blob),
        'required_exports_checked': len(required),
        'needed': sorted(needed),
    }
    setfile(member, blob)


# 仅调整解锁标记常量，保留全部指令和其余启动判据
bootlib = P + 'bootcontrol-v135/libdipper_bootcontrol_guarded_v135.so'
guard = old[bootlib][1]
assert sha(guard) == '92db6e9f567ba53854d5dd84b2143ce6f65fca9b9b7555d5b5fc39a4b8dc5430'
old_marker = b'SCREENLOCK_VERIFIED after=0 callback=0'
new_marker = b'SCREENLOCK_VERIFIED after=0 '
assert guard.count(old_marker) == 1
marker_offset = guard.index(old_marker)
replacement = new_marker + bytes(len(old_marker) - len(new_marker))
section = ELFFile(io.BytesIO(guard)).get_section_by_name('.rodata')
assert (
    section is not None
    and section['sh_offset'] <= marker_offset
    and marker_offset + len(old_marker) <= section['sh_offset'] + section['sh_size']
)
fixed_guard = guard[:marker_offset] + replacement + guard[marker_offset + len(old_marker) :]
setfile(bootlib, fixed_guard)
checksums = old[P + 'bootcontrol-v135/SHA256SUMS'][1].decode().replace(sha(guard), sha(fixed_guard))
setfile(P + 'bootcontrol-v135/SHA256SUMS', checksums)
setfile(
    G + 'mark-boot-success-v135.sh',
    (REPO / 'runtime/compat/graphics/mark-boot-success-v135.sh').read_bytes(),
    0o100755,
)
bootcontrol_report = {
    'input_sha256': sha(guard),
    'output_sha256': sha(fixed_guard),
    'offset': marker_offset,
    'before': old_marker.hex(),
    'after': replacement.hex(),
    'instructions_unchanged': True,
    'other_native_gates_unchanged': True,
    'exact_unlock_variants_checked_by_launcher': True,
}
recipes = {}
for path in sorted(recipe_paths):
    recipe = json.loads(path.read_text(encoding='utf-8'))
    source = ROOT / 'metadata/guard-inputs' / recipe['path'].lstrip('/')
    blob = source.read_bytes()
    assert sha(blob) == recipe['input_sha256']
    patched = bytearray(blob)
    for change in recipe['changes']:
        at = change['offset']
        before = bytes.fromhex(change['before'])
        after = bytes.fromhex(change['after'])
        assert len(before) == len(after) and patched[at : at + len(before)] == before
        patched[at : at + len(after)] = after
    assert sha(patched) == recipe['output_sha256']
    destination = WORK / 'patched' / recipe['path'].lstrip('/')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(patched)
    recipes[recipe['path']] = recipe
render_async_report = {'enabled': False}
if args.render_commit_async:
    # 仅在已验证IPC配对下恢复渲染提交异步标志
    async_recipe_path = REPO / 'patches/display/render-commit-async.json'
    async_recipe = json.loads(async_recipe_path.read_text(encoding='utf-8'))
    async_path = WORK / 'patched' / RENDER_COMMIT_ASYNC['path'].lstrip('/')
    restored, render_async_report = patch_render_commit_async(
        async_path.read_bytes(), async_recipe, ipc_input_report
    )
    async_path.write_bytes(restored)
    effective_recipe = copy.deepcopy(recipes[async_recipe['path']])
    effective_recipe['changes'].extend(async_recipe['changes'])
    effective_recipe['output_sha256'] = render_async_report['output_sha256']
    recipes[async_recipe['path']] = effective_recipe
file_migrate_report = {'enabled': False}
if args.file_migrate_hook_guard:
    file_migrate_path = FILE_MIGRATE_HOOK['path']
    records = json.loads((ROOT / 'metadata/system-files.json').read_text(encoding='utf-8'))
    file_migrate_report = validate_file_migrate_hook(
        recipes[file_migrate_path],
        records,
        (ROOT / 'metadata/guard-inputs' / file_migrate_path.lstrip('/')).read_bytes(),
    )
# 在启动归档内暂存补丁，不修改原始供体镜像
fields = [
    max(v[0][0] for v in new.values()) + 1,
    0o40755,
    0,
    0,
    2,
    0,
    0,
    0,
    0,
    0,
    0,
    len((G + 'dipper').encode()) + 1,
    0,
]
new[G + 'dipper'] = (fields, b'')
for path, recipe in recipes.items():
    if path == FILE_MIGRATE_HOOK['path']:
        if not args.file_migrate_hook_guard:
            raise ValueError('File-migrate hook requires its explicit option')
        continue
    name = path.rsplit('/', 1)[1]
    setfile(
        G + 'dipper/' + name,
        (WORK / 'patched' / path.lstrip('/')).read_bytes(),
        0o100755 if name == 'init' else 0o100644,
    )
# 同步替换兼容库及其输入哈希
for name in [
    'libaudio_policy_client.z.so',
    'libcode_sign_utils.z.so',
    'libsensor_client.z.so',
    'libsps_privacy_indicator_service_intf.z.so',
    'libwifi_sdk.z.so',
    'libnet_tether_manager_if.z.so',
]:
    matches = [(p, r) for p, r in recipes.items() if p.endswith('/' + name)]
    assert len(matches) == 1
    setfile(G + name, (WORK / 'patched' / matches[0][0].lstrip('/')).read_bytes())


# 安装构建期已完成补丁的文件，保留复制和挂载错误
def stage(path, target=None, bind=False):
    name = path.rsplit('/', 1)[1]
    target = target or '/hmos-root/compat/lib64/' + name
    source = '/hmos-root' + path
    result = (
        f'$B cp /graphics/dipper/{name} {target}\n'
        '$B chmod ' + ('0755' if name == 'init' else '0644') + f' {target}\n'
    )
    if bind:
        result += f'$B mount --bind {target} {source}\n'
    return result


head = '#!/busybox sh\nset -eu\nB=/busybox\nR=/hmos-root\n'
start = text('apply-compat-paths.sh').index('/busybox gzip -dc /graphics/libvulkan_freedreno.so.gz')
compat = (
    head
    + '$B mkdir -p $R/compat/bin $R/compat/lib64\n'
    + stage('/system/bin/init', '/hmos-root/compat/bin/init')
    + stage('/system/lib64/libvulkan.so')
    + text('apply-compat-paths.sh')[start:]
)
if args.present_fence_bridge:
    # 在原生显示服务启动前替换输出 fence 导出逻辑
    compat += stage('/vendor/lib64/libdisplay_composer_service_1.5.z.so', bind=True)
if args.file_migrate_hook_guard:
    compat = add_file_migrate_hook_guard(compat)
setfile(G + 'apply-compat-paths.sh', compat, 0o100755)
render = '/system/lib64/librender_service_base.z.so'
setfile(
    G + 'apply-rosen-handle.sh',
    head + stage(render) + 'echo DIPPER_ROSEN_PATCH_SET_STAGED\n',
    0o100755,
)
for name in ['apply-render-usage.sh', 'apply-rs-commit-sync-v1.sh']:
    setfile(
        G + name,
        head
        + 'echo DIPPER_ROSEN_PATCH_SET_STAGED\n',
        0o100755,
    )
setfile(
    G + 'apply-framework-dev.sh',
    head
    + stage('/system/lib64/libcode_sign_utils.z.so')
    + 'echo DIPPER_DEVELOPMENT_CODE_SIGN_COMPAT_STAGED\n',
    0o100755,
)
for script, path in [
    ('apply-cli-wait-v1.sh', '/system/lib64/platformsdk/libcli_tool_client.z.so'),
    ('apply-msdp-local-v1.sh', '/system/lib64/libdevicestatus_service.z.so'),
    ('apply-audio-wait-v1.sh', '/system/lib64/libaudio_policy_service.z.so'),
    ('apply-time-v1.sh', '/system/lib64/libtime_system_ability.z.so'),
    ('apply-memmgr-v2.sh', '/system/lib64/libmemmgrservice.z.so'),
]:
    body = head
    if script == 'apply-msdp-local-v1.sh':
        body += '$B sh /graphics/apply-storage-local-v1.sh\n'
    body += (
        stage(path, bind=True)
        + 'echo DIPPER_GUARDED_'
        + script.replace('-', '_').replace('.', '_')
        + '_STAGED\n'
    )
    setfile(G + script, body, 0o100755)
# 保留密钥目录准备流程，替换新版加密库
db = text('apply-db-key-v2.sh').split('S="$R/system/lib64/librelational_store_crypt.z.so"', 1)[0]
for name in [
    'librelational_store_crypt.z.so',
    'libkv_store_crypt.z.so',
    'libdistributeddatasvcfwk.z.so',
]:
    db += stage('/system/lib64/' + name, bind=True)
db += 'echo DIPPER_DB_KEY_BRIDGE_STAGED\n'
setfile(G + 'apply-db-key-v2.sh', db, 0o100755)
# 核对 Hks 接口及加密库导出符号
hmac = text('apply-account-hmac.sh')
account_checks = []
for path in [
    '/system/lib64/libaccountmgr.z.so',
    '/system/lib64/chipset-sdk/libhukssdk.z.so',
    '/system/lib64/chipset-sdk-sp/libcrypto_openssl.z.so',
]:
    data = (ROOT / 'metadata/guard-inputs' / path.lstrip('/')).read_bytes()
    digest = sha(data)
    line = next(l for l in hmac.splitlines() if path in l and 'sha256sum' in l)
    expected = re.search(r'\bin ([0-9a-f]{64})', line)[1]
    hmac = hmac.replace(expected, digest)
    account_checks.append({'path': path, 'input_sha256': digest})
huks = (ROOT / 'metadata/guard-inputs/system/lib64/chipset-sdk/libhukssdk.z.so').read_bytes()
symbols = ELFFile(io.BytesIO(huks)).get_section_by_name('.dynsym')
exports = {s.name for s in symbols.iter_symbols() if s['st_shndx'] != 'SHN_UNDEF'}
required = {'HksKeyExist', 'HksGenerateKey', 'HksInit', 'HksUpdate', 'HksFinish', 'HksAbort'}
assert required <= exports
setfile(G + 'apply-account-hmac.sh', hmac, 0o100755)
# 保留供体机型参数，仅关闭本机不支持的星闪
cap = (ROOT / 'metadata/guard-inputs/sys_prod/etc/param/hw_defaults.para').read_bytes()
assert cap.count(b'const.nearlink.enable=true') == 1
newcap = cap.replace(b'const.nearlink.enable=true', b'const.nearlink.enable=false')
# 机型形态按本机上报为手机，避免沿用供体的平板布局
if newcap.count(b'const.product.devicetype=tablet') != 1:
    raise ValueError('Unexpected donor device type')
newcap = newcap.replace(b'const.product.devicetype=tablet', b'const.product.devicetype=phone', 1)
setfile(G + 'hw_defaults-v141.para', newcap)
hardware = text('apply-hardware-v116.sh')
hardware = hardware.replace(
    '47e94ce212bf301531e1e0f6480f9b4c566296a557e8fc03b44d958ed81fe8a2', sha(cap)
).replace('3fa79d36281510333d96de8ff23844e40229b1d227ada18ca0eff2fbf24a9dc2', sha(newcap))
display_log_report = {'enabled': False}
if args.preserve_display_logs:
    tracked_hardware = REPO / 'runtime/compat/graphics/apply-hardware-v116.sh'
    hardware = enable_display_log_retention(
        hardware, tracked_hardware.read_text(encoding='utf-8')
    )
    display_log_report = {
        'enabled': True,
        'logs': [
            {'path': '/data/local/tmp/' + name, 'owner': owner, 'mode': mode}
            for name, owner, mode in DISPLAY_LOG_TARGETS
        ],
        'boundary': ['boot_id', 'uptime', 'pid'],
        'tracked_script_sha256': sha(tracked_hardware.read_bytes()),
        'log_failures_nonfatal': True,
        'runtime_validated': False,
    }
if args.refresh_helper:
    helper = args.refresh_helper.read_bytes()
    elf = ELFFile(io.BytesIO(helper))
    if sha(helper) != args.refresh_helper_sha256 or elf['e_machine'] != 'EM_AARCH64':
        raise ValueError('Refresh helper hash or architecture mismatch')
    setfile(G + 'dipper/native_refresh_rate', helper, 0o100755)
    hardware += (
        '$B cp /graphics/dipper/native_refresh_rate "$R/compat/bin/native_refresh_rate"\n'
        '$B chmod 0755 "$R/compat/bin/native_refresh_rate"\n'
        '$B touch "$R/data/local/tmp/dipper-refresh-request.txt" "$R/data/local/tmp/dipper-refresh-result.txt"\n'
        '$B chown 2000:2000 "$R/data/local/tmp/dipper-refresh-request.txt" "$R/data/local/tmp/dipper-refresh-result.txt"\n'
        '$B chmod 0600 "$R/data/local/tmp/dipper-refresh-request.txt" "$R/data/local/tmp/dipper-refresh-result.txt"\n'
    )
pcmode_stage = (
    '\n# 暂存用户域默认值，交给原设置服务初始化\n'
    '$B cp /graphics/dipper/default_settings-pcmode.json '
    '"$R/sys_prod/etc/settings_data/default_settings.json"\n'
    '$B chmod 0644 "$R/sys_prod/etc/settings_data/default_settings.json"\n'
)
if pcmode_stage not in hardware:
    hardware += pcmode_stage
setfile(G + 'apply-hardware-v116.sh', hardware, 0o100755)
preload = text('apply-preload.sh').replace('/preload/WKI-W10/all/cn', '/preload/GEM-W00/all/cn')
# 显式补入中国区资源层，恢复主题及动态图标
region = '/sys_prod/variant/region_comm/china'
file_index = json.loads((ROOT / 'metadata/sys_prod-files.json').read_text(encoding='utf-8'))
assert any(
    x['path'] == '/variant/region_comm/china/resource/themes/icon' and x['type'] == 'DIR'
    for x in file_index
)
if args.experimental_regional_resources:
    preload += (
        '\n$B printf "\\n# dipper: GEM-W00 regional resource policy\\n'
        '/sys_prod/variant/region_comm/china\\n" '
        '>> "$R/compat/etc/cust/baselayer.cfg"\n'
    )
setfile(G + 'apply-preload.sh', preload, 0o100755)
# 迁移沙箱兼容差异并保留新版字段
sandbox = text('apply-app-sandbox.sh')
sandbox_report = []
for name in ['appdata-sandbox.json', 'appdata-sandbox-isolated.json']:
    oldbytes = (ROOT / 'metadata/old-guard-inputs/system/etc/sandbox' / name).read_bytes()
    newbytes = (ROOT / 'metadata/guard-inputs/system/etc/sandbox' / name).read_bytes()
    patched_old = gzip.decompress(old[G + name + '.gz'][1])
    original = oldbytes.decode().splitlines(keepends=True)
    patched = patched_old.decode().splitlines(keepends=True)
    result = newbytes.decode()
    opcodes = difflib.SequenceMatcher(None, original, patched, autojunk=False).get_opcodes()
    edits = 0
    for tag, a, b, c, d in opcodes:
        if tag == 'equal':
            continue
        lo = max(0, a - 6)
        hi = min(len(original), b + 6)
        before = ''.join(original[lo:hi])
        after = ''.join(original[lo:a] + patched[c:d] + original[b:hi])
        assert result.count(before) == 1, (name, a, b, 'ambiguous sandbox context')
        result = result.replace(before, after, 1)
        edits += 1
    output = result.encode()
    assert (
        json.loads(output)['create-only-on-daemon'] == json.loads(newbytes)['create-only-on-daemon']
    )
    sandbox = sandbox.replace(sha(oldbytes), sha(newbytes)).replace(sha(patched_old), sha(output))
    setfile(G + name + '.gz', gzip.compress(output, mtime=0))
    sandbox_report.append(
        {
            'path': '/system/etc/sandbox/' + name,
            'input_sha256': sha(newbytes),
            'output_sha256': sha(output),
            'context_checked_edits': edits,
            'donor_additions_preserved': True,
        }
    )
setfile(G + 'apply-app-sandbox.sh', sandbox, 0o100755)
# 将音频策略覆盖挂载到供体现有目录
audio = text('apply-audio-v116.sh').replace(
    '[ ! -e "$R/chip_prod/etc/audio" ]\n$B ln -s /compat/etc/audio "$R/chip_prod/etc/audio"',
    '[ -d "$R/chip_prod/etc/audio" ]\n$B mount --bind "$R/compat/etc/audio" "$R/chip_prod/etc/audio"',
)
assert audio != text('apply-audio-v116.sh')
setfile(G + 'apply-audio-v116.sh', audio, 0o100755)
audio_payload_report = replace_audio_payload(new, args.audio_build_dir)
# 在派生持久参数配置前恢复服务管理器角色
samgr_inputs = {
    path: (ROOT / 'metadata/guard-inputs' / path.lstrip('/')).read_bytes()
    for path in SAMGR_INPUTS
}
samgr_config, samgr_role_report = restore_samgr_role(
    new[P + 'hmos-init-debug.cfg'][1], samgr_inputs
)
setfile(P + 'hmos-init-debug.cfg', samgr_config)
# 在派生私有参数配置前补齐原隐私服务，首次启动使用现有令牌目录
platform = json.loads(text('init-platform-v116.cfg'))
native_init = json.loads(new[P + 'hmos-init-debug.cfg'][1])
tracked_platform = json.loads(
    (REPO / 'runtime/compat/graphics/init-platform-v116.cfg').read_text(encoding='utf-8')
)
add_privacy_service(platform, native_init, tracked_platform)
setfile(G + 'init-platform-v116.cfg', json.dumps(platform, indent=2) + '\n')
setfile(P + 'hmos-init-debug.cfg', json.dumps(native_init, indent=2) + '\n')
# 延后启动桌面依赖服务，保留供体身份及权限
task_cfg_path = ROOT / 'metadata/guard-inputs/system/etc/init/task_manager_service.cfg'
task_profile_path = ROOT / 'metadata/guard-inputs/system/profile/task_manager_service.json'
task_cfg = json.loads(task_cfg_path.read_text(encoding='utf-8'))
task_profile = json.loads(task_profile_path.read_text(encoding='utf-8'))
assert task_profile['process'] == 'task_manager_service'
assert len(task_profile['systemability']) == 1 and task_profile['systemability'][0]['name'] == 65984
assert task_profile['systemability'][0]['depend'] == [3299]
task_service = task_cfg['services'][0]
assert (
    task_service['name'] == 'task_manager_service' and task_service['uid'] == 'task_manager_service'
)
assert task_service['path'] == ['/system/bin/sa_main', '/system/profile/task_manager_service.json']
# 使用供体要求的数值类型，避免中断后续权限解析
task_service['disabled'] = 1
assert task_profile['systemability'][0]['start-on-demand']['param'] == [
    {'name': 'const.taskmanager_service.support', 'value': 'true'}
]
task_profile['systemability'][0]['start-on-demand']['param'] = [
    {'name': 'dipper.debug.start.task', 'value': 'true'}
]
task_profile_bytes = (json.dumps(task_profile, indent=2) + '\n').encode()
setfile(G + 'dipper/task_manager_service.json', task_profile_bytes)
desktop = json.loads(text('init-desktop-services.cfg'))
assert all(s['name'] != 'task_manager_service' for s in desktop['services'])
desktop['services'].append(task_service)
prepare = next(j['cmds'] for j in task_cfg['jobs'] if j['name'] == 'post-fs-data')
desktop['jobs'].append(
    {
        'name': 'param:dipper-task',
        'condition': 'dipper.debug.start.screen=true',
        'cmds': prepare + ['setparam dipper.debug.start.task true'],
    }
)
# 校验原厂账户服务及用户定义，不替换认证实现
huawei_paths = {
    '/system/etc/init/huawei_id_svc.cfg': 'c9b4fa15727d18d3bd66ea7b00fe7e1c315d2574ed85604b8e2dc62306d32a2b',
    '/system/profile/huawei_id_svc.json': 'a2b00ede04d2c5d87c3a936aa2e979d0b250abf20057fd5b8cf069ac8ed9a3fa',
    '/system/lib64/libhuaweiid_svc.z.so': 'faa6d05f2c2e4fb933ebf4dc1f7ccc15f5a29bc7379b20329b23e646dbbdff14',
}
huawei_inputs = {}
for path, expected in huawei_paths.items():
    data = (ROOT / 'metadata/guard-inputs' / path.lstrip('/')).read_bytes()
    if sha(data) != expected:
        raise ValueError('HuaweiID donor hash mismatch: ' + path)
    huawei_inputs[path] = data
identity_files = {
    name: (ROOT / 'metadata/guard-inputs/system/etc' / name).read_bytes()
    for name in ['passwd', 'group']
}
passwd = next(
    line.split(':')
    for line in identity_files['passwd'].decode().splitlines()
    if line.startswith('huawei_id:')
)
groups = {
    line.split(':')[0]: line.split(':')[2]
    for line in identity_files['group'].decode().splitlines()
    if line.startswith(('huawei_id:', 'netsys_socket:'))
}
if passwd[2:4] != ['7008', '7008'] or groups != {
    'huawei_id': '7008',
    'netsys_socket': '1097',
}:
    raise ValueError('HuaweiID donor UID or GID mismatch')
huawei_cfg = json.loads(huawei_inputs['/system/etc/init/huawei_id_svc.cfg'])
huawei_profile = json.loads(huawei_inputs['/system/profile/huawei_id_svc.json'])
assert huawei_profile['systemability'][0]['name'] == 65926
add_huawei_id_service(desktop, huawei_cfg)
startup_input = text('start-native-desktop-v116.sh')
startup_output = add_huawei_id_startup(startup_input)
core_runtime_report = {'enabled': False}
experience_rule_report = {'enabled': False}
if args.core_runtime_services:
    core_inputs = {
        path: (ROOT / 'metadata/guard-inputs' / path.lstrip('/')).read_bytes()
        for path in CORE_RUNTIME_INPUTS
    }
    validate_core_runtime_inputs(core_inputs)
    donor_core = {
        'resource_cfg': json.loads(
            core_inputs['/system/etc/init/resource_schedule_service.cfg']
        ),
        'resource_profile': json.loads(
            core_inputs['/system/profile/resource_schedule_service.json']
        ),
        'foundation_cfg': json.loads(core_inputs['/system/etc/init/foundation.cfg']),
        'foundation_profile': json.loads(
            core_inputs['/system/profile/foundation.json']
        ),
    }
    original_foundation = new[G + 'foundation-app-core.json'][1]
    foundation_service = next(
        service
        for service in json.loads(new[P + 'hmos-init-debug.cfg'][1])['services']
        if service['name'] == 'foundation'
    )
    foundation, resource = add_core_runtime_services(
        desktop, json.loads(original_foundation), foundation_service, donor_core
    )
    foundation_bytes = (json.dumps(foundation, indent=2) + '\n').encode()
    resource_bytes = (json.dumps(resource, indent=2) + '\n').encode()
    setfile(G + 'foundation-app-core.json', foundation_bytes)
    setfile(G + 'dipper/foundation-app-core-baseline.json', original_foundation)
    setfile(G + 'dipper/resource-schedule-core.json', resource_bytes)
    startup_output = add_core_runtime_startup(startup_output)
    if args.experience_rule_service:
        # 单独校验可选清单，失败时保留已验证核心服务
        experience_rule_inputs = {
            path: (ROOT / 'metadata/guard-inputs' / path.lstrip('/')).read_bytes()
            for path in EXPERIENCE_RULE_INPUTS
        }
        experience_foundation = add_experience_rule_service(
            foundation, donor_core['foundation_profile'], experience_rule_inputs
        )
        experience_bytes = (json.dumps(experience_foundation, indent=2) + '\n').encode()
        setfile(G + 'dipper/foundation-experience-rule.json', experience_bytes)
        startup_output = add_experience_rule_startup(
            startup_output, sha(experience_bytes), sha(foundation_bytes)
        )
        experience_rule_report = {
            'enabled': True,
            'donor_inputs': EXPERIENCE_RULE_INPUTS,
            'sa': 6105,
            'process': 'foundation',
            'uid': 5523,
            'foundation_profile_sha256': sha(experience_bytes),
            'core_profile_sha256': sha(foundation_bytes),
            'donor_identity_preserved': True,
            'input_failure_retains_core_profile': True,
            'data_root': '/data/service/el1/public/ecological_rule_mgr_service',
            'runtime_validated': False,
        }
    core_runtime_report = {
        'enabled': True,
        'donor_inputs': CORE_RUNTIME_INPUTS,
        'added_foundation_sa': 3203,
        'resource_sas': [1901, 1903],
        'resource_uid': 1096,
        'resource_gids': [1096, 1000, 5001],
        'foundation_baseline_sha256': sha(original_foundation),
        'foundation_output_sha256': sha(foundation_bytes),
        'resource_profile_sha256': sha(resource_bytes),
        'startup_gate': 'dipper.debug.start.resourceschedule',
        'dependency_handling': 'original native service and SA profile declarations',
        'donor_permissions_preserved': True,
        'hardware_sysfs_jobs_imported': False,
        'runtime_validated': False,
    }
vibrator_report = {'enabled': False}
if args.vibrator_service:
    vibrator_inputs = {
        path: (ROOT / 'metadata/guard-inputs' / path.lstrip('/')).read_bytes()
        for path in VIBRATOR_INPUTS
    }
    validate_vibrator_inputs(vibrator_inputs)
    vibrator_profile = add_vibrator_service(
        desktop,
        json.loads(vibrator_inputs['/system/etc/init/sensors_musl.cfg']),
        json.loads(vibrator_inputs['/system/profile/sensors.json']),
    )
    vibrator_bytes = (json.dumps(vibrator_profile, indent=2) + '\n').encode()
    startup_output = add_vibrator_startup(startup_output)
    vibrator_report = {
        'enabled': True,
        'donor_inputs': VIBRATOR_INPUTS,
        'sas': [3602],
        'uid': 6688,
        'gids': [6688, 2000],
        'profile_sha256': sha(vibrator_bytes),
        'startup_gate': 'dipper.debug.start.vibrator',
        'donor_permissions_preserved': True,
        'hardware_sysfs_jobs_imported': False,
        'hardware_presence_validated': False,
        'runtime_validated': False,
    }
setfile(G + 'start-native-desktop-v116.sh', startup_output, 0o100755)
huawei_service_report = {
    'service': 'huawei_id_svc',
    'sa': 65926,
    'uid': 7008,
    'gid': [7008, 1097],
    'donor_inputs': huawei_paths,
    'identity_inputs': {name: sha(data) for name, data in identity_files.items()},
    'donor_permissions_preserved': True,
    'donor_prepare_job_preserved': True,
    'startup_gate': 'dipper.debug.start.huaweiid',
    'prerequisite_sas': [200, 401, 1151, 3299],
    'startup_input_sha256': sha(startup_input.encode()),
    'startup_output_sha256': sha(startup_output.encode()),
    'startup_failure_is_nonfatal': True,
    'authentication_binary_changes': False,
    'runtime_validated': False,
}
if args.refresh_helper:
    # 通过原生 init 分配身份，独立执行刷新率请求
    service = next(s.copy() for s in desktop['services'] if s['name'] == 'dipper_display_wake')
    service['name'] = 'dipper_refresh_rate'
    service['path'] = ['/compat/bin/native_refresh_rate']
    desktop['services'].append(service)
    desktop['jobs'].append(
        {
            'name': 'param:dipper-refresh-rate',
            'condition': 'dipper.debug.refresh.request=true',
            'cmds': ['start dipper_refresh_rate'],
        }
    )
# 独立生成原生事件载荷，保留既有服务和显式启动开关
hiview_report = {'enabled': False}
hiview_payload = None
if args.native_hiview_events:
    validate_hiview_init(json.loads(new[P + 'hmos-init-debug.cfg'][1]))
    hiview_inputs = {
        path: (ROOT / 'metadata/guard-inputs' / path.lstrip('/')).read_bytes()
        for path in HIVIEW_INPUTS
    }
    desktop, hiview_payload, hiview_report = prepare_hiview_payload(
        desktop, hiview_inputs, REPO, args.tools, WORK / 'native-hiview-events'
    )
    inspect_linux_bootstrap(hiview_payload['hiview_namespace_launcher'][0])
setfile(G + 'init-desktop-services.cfg', json.dumps(desktop, indent=2) + '\n')
# 保留核心清单，仅生成既有用户目录所需的备用服务定义
parent_control_report = {'enabled': False}
parent_control_payload = None
if args.parent_control_service:
    parent_control_inputs = {
        path: (ROOT / 'metadata/guard-inputs' / path.lstrip('/')).read_bytes()
        for path in PARENT_CONTROL_INPUTS
    }
    parent_control_cfg, parent_control_helper, parent_control_report = prepare_parent_control_payload(
        desktop, json.loads(new[P + 'hmos-init-debug.cfg'][1]), parent_control_inputs, REPO
    )
    parent_control_payload = {
        'init-desktop-parentcontrol.cfg': (parent_control_cfg, 0o100644),
        'prepare-parentcontrol-el1.sh': (parent_control_helper, 0o100755),
    }
task_service_report = {
    'service': 'task_manager_service',
    'sa': 65984,
    'donor_cfg_sha256': sha(task_cfg_path.read_bytes()),
    'donor_profile_sha256': sha(task_profile_path.read_bytes()),
    'staged_profile_sha256': sha(task_profile_bytes),
    'startup_gate': 'dipper.debug.start.screen -> prepare directories -> dipper.debug.start.task',
    'donor_permissions_preserved': True,
    'hardware_sysfs_jobs_imported': False,
    'runtime_validated': False,
}
# 添加供体软件区域参数，保持硬件标识不变
cust_parameters = {
    'const.cust.devmodel': 'GEM-W00',
    'const.cust.vendor': 'all',
    'const.cust.region': 'cn',
    'const.cust.home_region': 'cn',
}
cust_path = '/system/etc/param/dipper_cust.para'
assert not any(
    x['path'] == cust_path
    for x in json.loads((ROOT / 'metadata/system-files.json').read_text(encoding='utf-8'))
)
cust_output = (
    b'# dipper: verified GEM-W00 all/cn donor software layers\n'
    + ''.join(k + '=' + v + '\n' for k, v in cust_parameters.items()).encode()
)
if args.experimental_regional_resources:
    setfile(G + 'dipper/dipper_cust.para', cust_output)
cust_report = {
    'enabled': args.experimental_regional_resources,
    'path': cust_path,
    'new_file': True,
    'output_sha256': sha(cust_output),
    'parameters': cust_parameters,
    'explicit_regional_policy_layer': region if args.experimental_regional_resources else None,
    'hardware_identity_parameters_changed': False,
    'dynamic_icon_runtime_validated': False,
    'existing_theme_cache_may_require_reapply': True,
}
# 核对保存在 userdata 内的附加只读镜像
seed_names = ['preload', 'chip_prod', 'anco_hmos', 'cust', 'version']
seed_lines = []
for name in seed_names:
    folder = 'preload' if name == 'preload' else 'cust' if name == 'version' else 'base'
    img = ROOT / 'extracted' / folder / (name + '.img')
    report = json.loads(
        (ROOT / 'extracted' / folder / 'extraction-report.json').read_text(encoding='utf-8')
    )
    record = next(c for c in report['components'] if c['name'] == name)
    assert record['hash_verified'] and record['extracted']
    seed_lines.append(record['sha256'] + '  ' + name + '.img')
setfile(G + 'dipper/seed-images.sha256', '\n'.join(seed_lines) + '\n')
mount = '''#!/busybox sh
set -eu
B=/busybox
R=/hmos-root
S=$R/data/hmos-dipper
$B test -d "$S"
$B chown -R 0:0 "$S"
$B chmod 0700 "$S"
$B chmod 0600 "$S"/*.img
cd "$S"
$B sha256sum -c /graphics/dipper/seed-images.sha256
# 按内核实际设备号创建 loop 节点
for sysdir in /sys/class/block/loop[0-9]*; do
 name=${sysdir##*/}
 case "${name#loop}" in ''|*[!0-9]*) continue;; esac
 [ -r "$sysdir/dev" ] || continue
 nums=$($B cat "$sysdir/dev")
 $B rm -f "/dev/$name"
 $B mknod "/dev/$name" b "${nums%:*}" "${nums#*:}"
 $B chmod 0600 "/dev/$name"
done
$B test -b /dev/loop0
for name in preload chip_prod anco_hmos cust version; do
 lower=/hmos/dipper-$name
 $B mkdir -p "$lower" "$R/$name"
 loop=$($B losetup -f)
 $B losetup -r "$loop" "$S/$name.img"
 $B mount -t erofs -o ro,nosuid,nodev "$loop" "$lower"
 if [ "$name" = chip_prod ] || [ "$name" = preload ]; then
  upper=/hmos-ram/dipper-$name-upper
  work=/hmos-ram/dipper-$name-work
  $B mkdir -p "$upper" "$work"
  $B mount -t overlay -o lowerdir="$lower",upperdir="$upper",workdir="$work" overlay "$R/$name"
 else
  $B mount --bind "$lower" "$R/$name"
  $B mount -o remount,bind,ro,nosuid,nodev "$R/$name"
 fi
 echo "DIPPER_SEED_MOUNTED name=$name loop=$loop"
done
# 仅挂载安卓容器载荷，不启动后端
echo DIPPER_CONTAINER_FILES_PRESENT_RUNTIME_NOT_VALIDATED
'''
setfile(G + 'dipper/mount-seed-images.sh', mount, 0o100755)
userdata = text('init-userdata-ext4.sh') + '\n$B sh /graphics/dipper/mount-seed-images.sh\n'
private_persist_report = None
private_persist_payload = None
if args.private_persist_parameters:
    # 仅在用户数据挂载完成后准备镜像，默认配置始终保留
    candidate, helper, identities = prepare_private_persist(
        new[P + 'hmos-init-debug.cfg'][1],
        (REPO / 'runtime/compat/graphics/prepare-private-persist.sh').read_text(encoding='utf-8'),
        args.bootstrap_sha256,
    )
    private_persist_payload = (candidate, helper)
    userdata += (
        '\n# 完整准备参数镜像后才启用加载，失败继续原配置\n'
        f'if echo "{sha(helper)}  /graphics/dipper/prepare-private-persist.sh" '
        '| $B sha256sum -c -; then\n'
        ' if $B sh /graphics/dipper/prepare-private-persist.sh; then\n'
        '  private_persist_result=0\n'
        ' else\n'
        '  private_persist_result=$?\n'
        ' fi\n'
        'else\n'
        ' private_persist_result=1\n'
        'fi\n'
        'case "$private_persist_result" in\n'
        ' 0) echo HMOS_PRIVATE_PERSIST_COLD_READY;;\n'
        ' 1) echo HMOS_PRIVATE_PERSIST_BASELINE_RETAINED;;\n'
        ' # 无法确认回退完整时停止启动，保留参数和日志\n'
        ' *) echo HMOS_PRIVATE_PERSIST_HOLD >&2; exit 61;;\n'
        'esac\n'
    )
    private_persist_report = {
        'identities': identities,
        'helper_sha256': sha(helper),
        'native_init_command': 'load_private_persist_params',
        'existing_private_files_overwritten': False,
        'runtime_validated': False,
    }
setfile(G + 'init-userdata-ext4.sh', userdata, 0o100755)
userdata += (
    '$B cp /graphics/dipper/task_manager_service.json '
    '"$R/system/profile/task_manager_service.json"\n'
)
if args.experimental_regional_resources:
    userdata += (
        f'$B test ! -e "$R{cust_path}"\n'
        f'$B cp /graphics/dipper/dipper_cust.para "$R{cust_path}"\n'
        f'echo "{sha(cust_output)}  $R{cust_path}" | $B sha256sum -c -\n'
    )
if args.core_runtime_services:
    userdata += (
        '$B mkdir -p "$R/compat/etc"\n'
        '$B cp /graphics/dipper/resource-schedule-core.json '
        '"$R/compat/etc/resource-schedule-core.json"\n'
    )
setfile(G + 'init-userdata-ext4.sh', userdata, 0o100755)
if args.vibrator_service:
    setfile(G + 'dipper/sensors-vibrator-only.json', vibrator_bytes)
if args.file_migrate_hook_guard:
    # 延后暂存可选库，保留既有归档成员编号
    setfile(
        G + 'dipper/libfile_migrate_init.z.so',
        (WORK / 'patched' / FILE_MIGRATE_HOOK['path'].lstrip('/')).read_bytes(),
    )
# 最后分配测试成员，避免改变既有归档编号
if args.ipc_oneway_pid:
    ipc_report = replace_ipc_pair(new, ipc_module_blob, ipc_contract_blob)
if private_persist_payload:
    # 最后分配新增成员，保留现有归档编号
    candidate, helper = private_persist_payload
    setfile(G + 'dipper/hmos-init-private-persist.cfg', candidate)
    setfile(G + 'dipper/prepare-private-persist.sh', helper, 0o100755)
if hiview_payload:
    # 最后分配可选事件载荷，保持既有成员编号
    for name, (blob, mode) in hiview_payload.items():
        setfile(G + 'dipper/' + name, blob, mode)
if parent_control_payload:
    # 在桌面清单复制之后选择，失败保留核心和私有参数配置
    private_cfg, _ = private_persist_payload
    runtime_before = new[G + 'apply-runtime-v116.sh'][1].decode('utf-8')
    runtime_selected = add_parent_control_startup(
        runtime_before, sha(new[G + 'init-desktop-services.cfg'][1]),
        sha(parent_control_cfg), sha(parent_control_helper), sha(private_cfg),
    )
    setfile(G + 'apply-runtime-v116.sh', runtime_selected, 0o100755)
    for name, (blob, mode) in parent_control_payload.items():
        setfile(G + 'dipper/' + name, blob, mode)
    parent_control_report.update({
        'baseline_cfg_sha256': sha(new[G + 'init-desktop-services.cfg'][1]),
        'required_private_cfg_sha256': sha(private_cfg),
        'runtime_prefix_sha256': sha(runtime_before.encode()),
        'runtime_output_sha256': sha(runtime_selected.encode()),
        'selection_hook': 'apply-runtime-v116.sh after the original desktop copy',
    })
# 最后分配用户默认配置，保留既有归档编号
setfile(
    G + 'dipper/default_settings-pcmode.json',
    (REPO / 'runtime/compat/graphics/default_settings-pcmode.json').read_bytes(),
)
# 更新全部兼容组件的大小和哈希
manifest = json.loads(old[P + 'MANIFEST.json'][1])
members = set(manifest)
for name in new:
    if name.startswith(G + 'dipper/') and (new[name][0][1] & 0o170000) == 0o100000:
        members.add(name[len(P) :])
for relative in sorted(members):
    fields, blob = new[P + relative]
    manifest[relative] = {'bytes': len(blob), 'mode': oct(fields[1]), 'sha256': sha(blob)}
setfile(P + 'MANIFEST.json', json.dumps(manifest, indent=2) + '\n')
changed = {n for n in new if n not in old or new[n] != old[n]}
added = set(new) - set(old)
report = {
    'donor': 'GEM-LGRP3-CHN 207.0.0.111(SP12)',
    'base_vendor_boot_sha256': sha(base.read_bytes()),
    'changed_entries': sorted(changed),
    'added_entries': sorted(added),
    'patch_recipes': len(recipes),
    'account_input_checks': account_checks,
    'audio_payload': audio_payload_report,
    'seed_checksums': seed_lines,
    'boot_unchanged': True,
    'init_boot_unchanged': True,
    'dtb_unchanged': True,
    'kernel_unchanged': True,
    'device_boot_tested': False,
    'features_not_validated': ['Zhuoyitong Android backend', 'all application GPU paths'],
}
(WORK / 'payload-integration-report.json').write_text(
    json.dumps(report, indent=2) + '\n', encoding='utf-8'
)
report['sandbox_port'] = sandbox_report
report['dipper_task_service'] = task_service_report
report['huawei_id_service'] = huawei_service_report
report['samgr_original_role'] = samgr_role_report
report['core_runtime_services'] = core_runtime_report
report['experience_rule_service'] = experience_rule_report
report['native_hiview_events'] = hiview_report
report['parent_control_service'] = parent_control_report
report['vibrator_service'] = vibrator_report
report['bootcontrol_unlock_marker'] = bootcontrol_report
report['donor_software_layers'] = cust_report
report['display_composer'] = composer_report
report['display_allocator'] = allocator_report
report['present_fence_bridge'] = args.present_fence_bridge
report['bootstrap'] = bootstrap_report
report['ipc_pair'] = ipc_report
report['render_commit_async'] = render_async_report
report['display_log_retention'] = display_log_report
report['file_migrate_hook_guard'] = file_migrate_report
report['private_persist_parameters'] = private_persist_report
# 导出生成脚本并检查 Shell 语法
scripts = WORK / 'integrated-scripts'
scripts.mkdir(exist_ok=True)
for n in changed:
    if n.endswith('.sh'):
        dest = scripts / n.rsplit('/', 1)[1]
        dest.write_bytes(new[n][1])
        subprocess.run([r'C:\Program Files\Git\bin\bash.exe', '-n', str(dest)], check=True)


# 仅替换已核对的归档成员
def mutate(entries):
    for name in sorted(changed):
        entries[name] = new[name]


image = vr.edit(
    base, report['base_vendor_boot_sha256'], WORK / 'vendor-boot-repack', mutate, changed, added
)
destination = WORK / 'vendor_boot_dipper_111sp12.img'
destination.write_bytes(image)
report['vendor_boot_sha256'] = sha(image)
report['vendor_boot_bytes'] = len(image)
report['cpio_roundtrip_verified'] = True

# 米 8 的兼容层载荷不再放 vendor_boot，而是落在 cust 分区。cust 上的
# /cust/compat 就是载荷根：启动时它整体 bind 到 /compat，其中的 graphics、
# busybox、hmos-init-debug.cfg 再分别 bind 到 initramfs 根。所以导出映射是：
#   dipper-compat-v118/graphics/x -> compat/graphics/x
#   compat/x                      -> compat/x
#   graphics/x, busybox, hmos-init-debug.cfg -> compat/<同名>
# 其余（init、lib/modules 等）留在 boot 的 ramdisk 里，不重复导出。
def payload_relative(name):
    if name.startswith(P):
        return 'compat/' + name[len(P):]
    if name.startswith('compat/'):
        return name
    if name in ('busybox', 'hmos-init-debug.cfg') or name.startswith('graphics/'):
        return 'compat/' + name
    return None


if args.payload_out:
    entries = vr.parse((WORK / 'vendor-boot-repack/ramdisk.cpio').read_bytes())
    root = args.payload_out
    if root.exists() and any(root.iterdir()):
        raise SystemExit(f'{root} is not empty; refusing to overwrite a payload tree')
    files, links, skipped = 0, 0, []
    for name in sorted(entries):
        relative = payload_relative(name)
        if relative is None:
            skipped.append(name)
            continue
        fields, blob = entries[name]
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        mode = fields[1]
        if stat.S_ISLNK(mode):
            target.unlink(missing_ok=True)
            target.symlink_to(blob.decode('utf-8', 'surrogateescape'))
            links += 1
            continue
        if not stat.S_ISREG(mode):
            continue
        target.write_bytes(blob)
        target.chmod(mode & 0o7777)
        files += 1
    init = root / 'compat/bin/init'
    if not init.is_file():
        raise SystemExit('payload export has no compat/bin/init; the second stage would '
                         'not start. Check the base ramdisk layout before repacking.')
    report['payload_out'] = {
        'root': str(root), 'files': files, 'symlinks': links,
        'skipped_from_payload': skipped,
        'layout': 'compat/{bin,lib64,etc,graphics,busybox,hmos-init-debug.cfg}',
    }
    print(f'payload tree exported: {files} files, {links} symlinks under {root}', flush=True)

(WORK / 'payload-integration-report.json').write_text(
    json.dumps(report, indent=2) + '\n', encoding='utf-8'
)
print(json.dumps(report, indent=2), flush=True)

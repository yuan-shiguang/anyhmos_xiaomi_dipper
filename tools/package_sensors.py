"""Add the persistent SSC backend and original sensor services to a vendor boot."""

import argparse
import copy
import hashlib
import json
import stat
import struct
import subprocess
from pathlib import Path

import vendor_ramdisk_edit as editor


REPO = Path(__file__).resolve().parents[1]
COMPAT = 'dipper-compat-v118/'
GRAPHICS = COMPAT + 'graphics/'
PACKAGE = 'dipper-sensors'
READY = 'bootevent.dipper_sensor_hdi.ready'
PROFILE = GRAPHICS + 'dipper/sensors-vibrator-only.json'
SCRIPTS = ('start-sensors.sh', 'prepare-private-persist.sh', 'stage-fastrpc-nodes.sh')
BINARIES = ('sensor_hdi_publisher', 'libsensor_interface_service_3.1.z.so',
            'native_sensorspd_launcher')
EXECUTABLES = {'sensor_hdi_publisher', 'native_sensorspd_launcher', *SCRIPTS}
PARCEL_PATCHES = (
    (0x1285C, bytes.fromhex('81020291'), bytes.fromhex('81e20191')),
    (0x12884, bytes.fromhex('81e20191'), bytes.fromhex('81f20191')),
    (0x128AC, bytes.fromhex('81f20191'), bytes.fromhex('81020291')),
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def donor_file(root, relative):
    path = root / relative
    if not path.is_file() and relative.startswith('system/'):
        path = root / 'system' / relative
    return path.read_bytes()


def patch_sensor_parcel(original):
    # 仅修正原读端三条字段地址，保留二进制补丁的指令前像
    result = bytearray(original)
    for offset, before, after in PARCEL_PATCHES:
        if original[offset:offset + 4] != before:
            raise ValueError('Sensor parcel instruction differs at ' + hex(offset))
        result[offset:offset + 4] = after
    return bytes(result)


def configure_services(config, donor_host, donor_sensors):
    config = copy.deepcopy(config)
    host = copy.deepcopy(donor_host)
    host.update(path=['/compat/bin/native_sensor_hdi_publisher',
                      '/compat/lib64/libsensor_interface_service_3.1.z.so', '--serve'],
                disabled=1, once=1)
    selected = next((s for s in config['services'] if s['name'] == 'sensor_host'), None)
    if selected is None:
        config['services'].append(host)
    else:
        selected.clear()
        selected.update(host)
    sensors = next((s for s in config['services'] if s['name'] == 'sensors'), None)
    if sensors is None:
        sensors = copy.deepcopy(donor_sensors)
        config['services'].append(sensors)
    sensors.update(path=['/system/bin/sa_main', '/compat/etc/sensors-vibrator-only.json'],
                   disabled=1, once=1, saspawn=False, **{'start-mode': 'condition'})

    start = next((j for j in config['jobs']
                  if j.get('condition') == 'dipper.debug.start.vibrator=true'), None)
    if start is None:
        start = {'name': 'param:dipper-vibrator-service',
                 'condition': 'dipper.debug.start.vibrator=true',
                 'cmds': ['mkdir /data/service/el1/public/sensor',
                          'chown sensor sensor /data/service/el1/public/sensor']}
        config['jobs'].append(start)
    start['cmds'] = [c for c in start['cmds'] if c not in ('start sensors', 'start sensor_host')]
    start['cmds'].append('start sensor_host')
    stop = next((j for j in config['jobs']
                 if j.get('condition') == 'dipper.debug.start.vibrator=false'), None)
    if stop is None:
        stop = {'name': 'param:dipper-vibrator-service-stop',
                'condition': 'dipper.debug.start.vibrator=false', 'cmds': []}
        config['jobs'].append(stop)
    stop['cmds'] = [c for c in stop['cmds'] if c not in ('stop sensors', 'stop sensor_host')]
    stop['cmds'].extend(['stop sensors', 'stop sensor_host'])
    config['jobs'] = [j for j in config['jobs'] if j.get('name') not in
                      ('param:dipper-sensor-hdi-ready', 'param:dipper-sensor-hdi-stopped')]
    # 原系统服务只在真实枚举及HDI发布完成后启动
    config['jobs'].extend([
        {'name': 'param:dipper-sensor-hdi-ready',
         'condition': READY + '=true && dipper.debug.start.vibrator=true',
         'cmds': ['start sensors']},
        {'name': 'param:dipper-sensor-hdi-stopped', 'condition': READY + '=false',
         'cmds': ['stop sensors']},
    ])
    return config


def audio_startup(source):
    # 米 8 的音频脚本不再依赖 ADSP remoteproc 编号，改为在音频卡检测结束后挂传感器
    anchor = 'AUDIO_CARD_CHECKED=1\n'
    if source.count(anchor) != 1:
        raise ValueError('Audio card startup point is missing or ambiguous')
    # 替换旧隔离入口，保留原音频顺序与其余启动命令
    lines = source.splitlines(keepends=True)
    lines = [line for line in lines if not (
        ('setsid' in line and ('start-sensors-cold.sh' in line or 'start-sensors.sh' in line))
        or line.startswith('# ADSP首次就绪后异步采集传感器冷启动')
        or line.startswith('# ADSP就绪后启动常驻传感器后端'))]
    source = ''.join(lines)
    block = ('# 音频卡检测后异步启动传感器，不阻塞原音频\n'
             f'SENSOR_STARTUP_PACKAGE=/{PACKAGE} $B setsid $B sh /{PACKAGE}/start-sensors.sh '
             '>> "$D/sensor-startup-bootstrap.log" 2>&1 </dev/null &\n')
    return source.replace(anchor, anchor + block, 1)


def stage_commands():
    return f'''
# 安装原生传感器实现及完整系统服务清单
$B mkdir -p "$R/compat/bin" "$R/compat/lib64" "$R/compat/etc"
$B cp /{PACKAGE}/sensor_hdi_publisher "$R/compat/bin/native_sensor_hdi_publisher"
$B cp /{PACKAGE}/libsensor_interface_service_3.1.z.so "$R/compat/lib64/libsensor_interface_service_3.1.z.so"
$B cp /{PACKAGE}/sensors.json "$R/compat/etc/sensors-vibrator-only.json"
$B chmod 0755 "$R/compat/bin/native_sensor_hdi_publisher"
$B chmod 0644 "$R/compat/lib64/libsensor_interface_service_3.1.z.so" "$R/compat/etc/sensors-vibrator-only.json"
$B cp /{PACKAGE}/libsensor_utils.z.so "$R/compat/lib64/libsensor_utils.z.so"
$B chmod 0644 "$R/compat/lib64/libsensor_utils.z.so"
$B mount --bind "$R/compat/lib64/libsensor_utils.z.so" "$R/system/lib64/platformsdk/libsensor_utils.z.so"
# 保留原参数权限，只增加传感器进程的精确就绪键
$B mkdir -p "$R/compat/sensor-param"
$B cp -a "$R/system/etc/param/." "$R/compat/sensor-param/"
echo '{READY} = sensor_host:sensor_host:0775:bool' > "$R/compat/sensor-param/dipper_sensor.para.dac"
$B chmod 0644 "$R/compat/sensor-param/dipper_sensor.para.dac"
$B mount --bind "$R/compat/sensor-param" "$R/system/etc/param"
'''


def replace_runtime_stage(source):
    end = '$B mount --bind "$R/compat/sensor-param" "$R/system/etc/param"\n'
    for marker in ('# 安装传感器原生HDI实现，保留供体代理和系统服务\n',
                   '# 安装原生传感器实现及完整系统服务清单\n'):
        if marker in source:
            start = source.index(marker)
            finish = source.index(end, start) + len(end)
            source = source[:start] + source[finish:]
    anchor = 'R=/hmos-root\n'
    if source.count(anchor) != 1:
        raise ValueError('Runtime installation point is missing or ambiguous')
    return source.replace(anchor, anchor + stage_commands(), 1)


def make_mutator(donor_root, build_dir, stock_assets):
    profile = donor_file(donor_root, 'system/profile/sensors.json')
    host = json.loads(donor_file(donor_root, 'chip_prod/etc/init/hdf_devhost/sensor_host.cfg'))['services'][0]
    sensors = json.loads(donor_file(donor_root, 'system/etc/init/sensors_musl.cfg'))['services'][0]
    payloads = {name: (build_dir / name).read_bytes() for name in BINARIES}
    payloads.update({name: (REPO / 'runtime/compat/sensors' / name).read_bytes() for name in SCRIPTS})
    payloads['root.tar.gz'] = stock_assets.read_bytes()
    payloads['sensors.json'] = profile
    payloads['libsensor_utils.z.so'] = patch_sensor_parcel(
        donor_file(donor_root, 'system/lib64/platformsdk/libsensor_utils.z.so'))

    def mutate(entries):
        original = {name: data for name, (_, data) in entries.items()}

        def put(name, data, executable=False):
            if isinstance(data, str):
                data = data.encode()
            if name in entries:
                fields = entries[name][0].copy()
            else:
                fields = [max(f[0] for f, _ in entries.values()) + 1,
                          stat.S_IFREG | (0o755 if executable else 0o644),
                          0, 0, 1, 0, len(data), 0, 0, 0, 0, 0, 0]
            entries[name] = (fields, data)

        if PACKAGE not in entries:
            inode = max(f[0] for f, _ in entries.values()) + 1
            entries[PACKAGE] = ([inode, stat.S_IFDIR | 0o755, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0], b'')
        for name, data in payloads.items():
            put(PACKAGE + '/' + name, data, name in EXECUTABLES)
        put(PROFILE, profile)
        apply_name = GRAPHICS + 'apply-runtime-v116.sh'
        apply = entries[apply_name][1].decode()
        for name in [GRAPHICS + 'init-desktop-services.cfg',
                     GRAPHICS + 'dipper/init-desktop-cert-manager.cfg']:
            if name not in entries:
                continue
            config = configure_services(json.loads(entries[name][1]), host, sensors)
            put(name, json.dumps(config, ensure_ascii=False, indent=4) + '\n')
            apply = apply.replace(sha(original[name]), sha(entries[name][1]))
        put(apply_name, replace_runtime_stage(apply))
        audio_name = GRAPHICS + 'start-audio-v116.sh'
        put(audio_name, audio_startup(entries[audio_name][1].decode()))
        desktop_name = GRAPHICS + 'start-native-desktop-v116.sh'
        desktop = entries[desktop_name][1].decode()
        if PROFILE in original:
            desktop = desktop.replace(sha(original[PROFILE]), sha(profile))
        # 未选择旧振动入口的基础包也从原参数任务启动
        if 'param set dipper.debug.start.vibrator true' not in desktop:
            anchor = 'stage=scene\ntrace_stage\n'
            if desktop.count(anchor) != 1:
                raise ValueError('Desktop service startup point is missing or ambiguous')
            desktop = desktop.replace(anchor, '$B chroot "$R" /system/bin/param set '
                                      'dipper.debug.start.vibrator true\n' + anchor, 1)
        put(desktop_name, desktop)
        manifest_name = COMPAT + 'MANIFEST.json'
        manifest = json.loads(entries[manifest_name][1])
        for name, (fields, data) in entries.items():
            if name.startswith(COMPAT) and original.get(name) != data and name != manifest_name:
                relative = name[len(COMPAT):]
                record = manifest.setdefault(relative, {'mode': oct(fields[1])})
                record.update(bytes=len(data), sha256=sha(data))
        put(manifest_name, json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')

    return mutate, payloads


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vendor-boot', type=Path, required=True,
                        help='Base vendor_boot with the dipper graphics/audio compatibility layer')
    parser.add_argument('--donor-root', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--stock-assets', type=Path, required=True, help='External private SSC root tar.gz')
    parser.add_argument('--magiskboot', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    work = args.output.parent / (args.output.stem + '-repack')
    editor.MAGISKBOOT = str(args.magiskboot.resolve())
    mutate, payloads = make_mutator(args.donor_root, args.build_dir, args.stock_assets)
    # 先读原归档确定新增成员，继续沿既有编辑器保留其他镜像区段
    allowed = {PROFILE, GRAPHICS + 'apply-runtime-v116.sh', GRAPHICS + 'start-audio-v116.sh',
               GRAPHICS + 'start-native-desktop-v116.sh', GRAPHICS + 'init-desktop-services.cfg',
               GRAPHICS + 'dipper/init-desktop-cert-manager.cfg', COMPAT + 'MANIFEST.json',
               PACKAGE, *[PACKAGE + '/' + name for name in payloads]}
    base = args.vendor_boot.read_bytes()
    # 编辑器的新增清单需在解压后按实际原成员计算
    work.mkdir(exist_ok=True)
    page = struct.unpack_from('<I', base, 12)[0]
    header = struct.unpack_from('<I', base, 2096)[0]
    start = (header + page - 1) // page * page
    size = struct.unpack_from('<I', base, 24)[0]
    archive = work / 'input.ramdisk'
    archive.write_bytes(base[start:start + size])
    subprocess.run([editor.MAGISKBOOT, 'decompress', str(archive), str(work / 'input.cpio')],
                   check=True, capture_output=True)
    before = editor.parse((work / 'input.cpio').read_bytes())
    probe = {name: (fields.copy(), data) for name, (fields, data) in before.items()}
    mutate(probe)
    added = set(probe) - set(before)
    image = editor.edit(args.vendor_boot, sha(base), work, mutate, allowed, added)
    args.output.write_bytes(image)
    report = {'image': str(args.output), 'sha256': sha(image), 'bytes': len(image),
              'changed_members': sorted(n for n in before if probe[n] != before[n]),
              'added_members': sorted(added), 'payloads': {n: sha(b) for n, b in payloads.items()},
              'cold_boot_verified': False, 'factory_reset_verified': False}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'image': str(args.output), 'sha256': sha(image), 'bytes': len(image)}))


if __name__ == '__main__':
    main()

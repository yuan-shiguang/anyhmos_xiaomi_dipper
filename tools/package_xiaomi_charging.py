import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

import vendor_ramdisk_edit as editor

ROOT = Path(__file__).resolve().parent.parent
PREFIX = 'dipper-compat-v118/'
DIRECTORY = PREFIX + 'graphics/xiaomi-charging'
AUDIO = PREFIX + 'graphics/start-audio-v116.sh'
MANIFEST = PREFIX + 'MANIFEST.json'
HOOK = (
    '\n# 音频初始化完成后启动原厂充电认证\n'
    '$B setsid $B sh /graphics/xiaomi-charging/start-xiaomi-charging.sh '
    '>/dev/null 2>&1 </dev/null &\n'
).encode('utf-8')


# 读取基镜像成员以复用目录元数据
def read_base(base, work, magiskboot):
    image = base.read_bytes()
    page = struct.unpack_from('<I', image, 12)[0]
    header_size = struct.unpack_from('<I', image, 2096)[0]
    ramdisk_size = struct.unpack_from('<I', image, 24)[0]
    offset = (header_size + page - 1) // page * page
    compressed = work / 'base.lz4'
    raw = work / 'base.cpio'
    compressed.write_bytes(image[offset:offset + ramdisk_size])
    subprocess.run(
        [str(magiskboot), 'decompress', str(compressed), str(raw)],
        check=True, capture_output=True,
    )
    return editor.parse(raw.read_bytes())


# 仅追加充电启动命令并更新对应成员清单
def package(args):
    args.work.mkdir(parents=True, exist_ok=True)
    repack = args.work / 'repack'
    repack.mkdir(exist_ok=True)
    original = read_base(args.base, repack, args.magiskboot)
    inputs = {
        'batterysecret': (args.payload / 'batterysecret', 0o100755),
        'libbinder.so': (args.payload / 'libbinder.so', 0o100644),
        'android.hardware.health@1.0.so': (
            args.payload / 'android.hardware.health@1.0.so', 0o100644,
        ),
        'libdipper_batterysecret_process.so': (args.shim, 0o100644),
        'start-xiaomi-charging.sh': (
            ROOT / 'runtime/compat/graphics/start-xiaomi-charging.sh', 0o100755,
        ),
    }
    payload = {
        DIRECTORY + '/' + name: (path.read_bytes(), mode)
        for name, (path, mode) in inputs.items()
    }
    allowed = {DIRECTORY, AUDIO, MANIFEST, *payload}
    added = allowed.difference(original)
    next_inode = max(fields[0] for fields, _ in original.values()) + 1
    changes = []

    def mutate(entries):
        nonlocal next_inode

        def put(name, data, mode):
            nonlocal next_inode
            template = AUDIO if mode & 0o100000 else PREFIX + 'graphics'
            fields = entries[name][0].copy() if name in entries else entries[template][0].copy()
            if name not in entries:
                fields[0] = next_inode
                next_inode += 1
            fields[1] = mode
            fields[4] = 1 if mode & 0o100000 else 2
            fields[6] = len(data)
            entries[name] = (fields, data)

        if DIRECTORY not in entries:
            put(DIRECTORY, b'', 0o40755)
        for name, (data, mode) in payload.items():
            put(name, data, mode)

        fields, audio = entries[AUDIO]
        if not audio.endswith(HOOK):
            entries[AUDIO] = (fields.copy(), audio + HOOK)

        manifest = json.loads(entries[MANIFEST][1])
        for name in [AUDIO, *payload]:
            fields, data = entries[name]
            manifest[name.removeprefix(PREFIX)] = {
                'bytes': len(data),
                'mode': oct(fields[1]),
                'sha256': hashlib.sha256(data).hexdigest(),
            }
        fields, _ = entries[MANIFEST]
        entries[MANIFEST] = (
            fields.copy(), (json.dumps(manifest, indent=2) + '\n').encode('utf-8'),
        )

        for name in sorted(allowed):
            if entries.get(name) == original.get(name):
                continue
            fields, data = entries[name]
            changes.append({
                'member': name,
                'added': name not in original,
                'bytes': len(data),
                'mode': oct(fields[1]),
                'sha256': hashlib.sha256(data).hexdigest(),
            })

    editor.MAGISKBOOT = str(args.magiskboot)
    image = editor.edit(args.base, args.base_sha256, repack, mutate, allowed, added)
    output = args.work / 'vendor_boot_xiaomi_charging.img'
    output.write_bytes(image)
    report = {
        'base': str(args.base),
        'base_sha256': args.base_sha256,
        'output': str(output),
        'sha256': hashlib.sha256(image).hexdigest(),
        'bytes': len(image),
        'changed_members': changes,
        'audio_original_prefix_preserved': True,
    }
    (args.work / 'package-result.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n',
        encoding='utf-8', newline='\n',
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--base-sha256', required=True)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--shim', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--magiskboot', type=Path, required=True)
    package(parser.parse_args())


if __name__ == '__main__':
    main()

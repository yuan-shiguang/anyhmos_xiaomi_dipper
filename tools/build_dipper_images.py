"""Pack the dipper image set for a single-slot static-partition device.

This tool only writes host files. It never opens or flashes a connected device.

米 8 没有 super、vendor_boot、init_boot：system/vendor 是静态分区，兼容层载荷放在
cust。所以这里不做 lpmake，也不碰 LP 元数据，只做四件事：

  1. 按 tools/dipper_partitions.json 读出真实容量；
  2. 把 donor 的 system/vendor 镜像按容量校验后拷进输出目录（超限直接报错，不截断）；
  3. 用 mke2fs 把载荷树打成 cust 的 ext4 镜像；
  4. 校验 boot.img 头，按 64 MiB 上限检查后拷贝。

分区容量可以用 --system-bytes/--vendor-bytes/--cust-bytes 覆盖，用于已经扩容的机器。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import time
from pathlib import Path

from android_sparse import SparseImage

LAYOUT_FILE = Path(__file__).resolve().parent / 'dipper_partitions.json'
SPARSE_MAGIC = b'\x3a\xff\x26\xed'
EXT4_MAGIC = b'\x53\xef'
EROFS_MAGIC = b'\xe2\xe1\xf5\xe0'
F2FS_MAGIC = b'\x10\x20\xf5\xf2'
ANDROID_MAGIC = b'ANDROID!'
# 米 8 上这些分区不存在；构建器一旦发现它们出现在分区表里，说明布局数据被换成了别的机型
ABSENT = ('super', 'vendor_boot', 'init_boot', 'system_ext', 'product', 'odm',
          'vendor_dlkm', 'metadata', 'vbmeta_system')
REQUIRED = ('system', 'vendor', 'cust', 'boot')


def device_layout(path=LAYOUT_FILE):
    layout = json.loads(Path(path).read_text(encoding='utf-8'))
    return layout, {p['label']: p for p in layout['partitions']}


def assert_static_layout(partitions):
    """分区表必须是米 8 的单槽静态布局，否则后面所有容量校验都是错的。"""
    present = [name for name in ABSENT if name in partitions]
    if present:
        raise SystemExit(
            'Partition table still lists ' + ', '.join(present)
            + '. dipper is a single-slot static-partition device; a super/vendor_boot '
              'layout means the wrong partition data was loaded. See docs/DEVICE-PARTITIONS.md.'
        )
    missing = [name for name in REQUIRED if name not in partitions]
    if missing:
        raise SystemExit('Partition table is missing ' + ', '.join(missing)
                         + '; refusing to build an image set that cannot be flashed.')


def capacity(partitions, name, override):
    """容量以分区表为准；--*-bytes 用于已扩容的机器。"""
    declared = partitions[name]['size_bytes']
    if override is None:
        return declared
    if override % 4096 or override < declared:
        raise SystemExit(f'{name}: --{name}-bytes must be a 4 KiB multiple and no smaller '
                         f'than the {declared} bytes in the partition table')
    return override


def image_extent(path):
    """返回 (文件字节, 展开后字节, 格式)。sparse 镜像按展开后的大小做容量校验。"""
    path = Path(path)
    file_bytes = path.stat().st_size
    with path.open('rb') as stream:
        head = stream.read(4)
    if head != SPARSE_MAGIC:
        return file_bytes, file_bytes, 'raw'
    # SparseImage 会逐块校验 chunk 表，顺便给出真实的展开大小
    image = SparseImage(path)
    try:
        return file_bytes, image.size, 'sparse'
    finally:
        image.close()


def fit(name, need, have):
    if need <= have:
        return
    raise SystemExit(
        f'{name} payload is {need} bytes but the partition holds {have} bytes '
        f'({need - have} bytes over). Nothing was truncated.\n'
        f'  - system: enlarge the partition, then rerun with --system-bytes <capacity>\n'
        f'  - vendor/cust: shrink the payload or repartition\n'
        f'  See docs/DEVICE-PARTITIONS.md for the current table and the budget notes.'
    )


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def filesystem_of(path):
    """识别镜像里真正的文件系统：probe 只能挂 ext4/erofs/f2fs，别的挂了也起不来。"""
    path = Path(path)
    with path.open('rb') as stream:
        if stream.read(4) == SPARSE_MAGIC:
            return 'sparse'
        stream.seek(1024)
        head = stream.read(4)
        if head == EROFS_MAGIC:
            return 'erofs'
        if head == F2FS_MAGIC:
            return 'f2fs'
        stream.seek(1080)
        if stream.read(2) == EXT4_MAGIC:
            return 'ext4'
    return 'unknown'


def copy_checked(source, target, name, have):
    file_bytes, expanded, kind = image_extent(source)
    fit(name, expanded, have)
    filesystem = filesystem_of(source)
    if filesystem in ('unknown',):
        raise SystemExit(f'{name}: {source} is neither ext4, erofs nor f2fs; the probe '
                         'cannot mount it, so packing it would only produce an unbootable image')
    shutil.copyfile(source, target)
    if sha256(source) != sha256(target):
        raise SystemExit(f'{name}: copy verification failed')
    return {'path': f'images/{target.name}', 'bytes': file_bytes, 'sha256': sha256(target),
            'expanded_bytes': expanded, 'format': kind, 'filesystem': filesystem,
            'capacity_bytes': have}


def build_cust(payload, target, have, mke2fs, work):
    """载荷树打成 ext4；probe 会把它挂成可写并从 /cust/compat 取载荷。"""
    payload = Path(payload)
    for required in ('compat',):
        if not (payload / required).is_dir():
            raise SystemExit(f'payload root {payload} has no {required}/ directory')
    blocks = have // 4096
    if (payload / 'hmos').is_dir():
        staged = 'sys_prod/preload'
    else:
        staged = 'none'
    command = [mke2fs, '-t', 'ext4', '-b', '4096', '-I', '256', '-m', '0',
               '-L', 'cust', '-d', str(payload), str(target), str(blocks)]
    log = Path(work) / 'mke2fs-cust.log'
    with log.open('wb') as out:
        result = subprocess.run(command, stdout=out, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'mke2fs failed ({result.returncode}); see {log}')
    if target.stat().st_size != have:
        raise SystemExit(f'cust image is {target.stat().st_size} bytes, expected {have}')
    with target.open('rb') as stream:
        stream.seek(1024)
        superblock = stream.read(1024)
    if superblock[56:58] != EXT4_MAGIC:
        raise SystemExit('cust image has no ext4 superblock magic')
    declared_blocks = struct.unpack('<I', superblock[4:8])[0]
    if declared_blocks != blocks:
        raise SystemExit(f'cust image declares {declared_blocks} blocks, expected {blocks}')
    return {'path': f'images/{target.name}', 'bytes': target.stat().st_size,
            'sha256': sha256(target), 'expanded_bytes': target.stat().st_size,
            'format': 'raw-ext4', 'capacity_bytes': have, 'staged_partitions': staged}


def check_boot(source, have):
    """boot 只做头部与容量校验：内核来自 4.9 源码树，本工具不重打包。"""
    file_bytes, expanded, kind = image_extent(source)
    if kind != 'raw':
        raise SystemExit('boot.img must be a raw Android boot image, not sparse')
    with Path(source).open('rb') as stream:
        header = stream.read(64)
    if header[:8] != ANDROID_MAGIC:
        raise SystemExit('boot.img has no ANDROID! magic')
    kernel_bytes, ramdisk_bytes = struct.unpack('<II', header[8:16])
    page = struct.unpack('<I', header[36:40])[0]
    version = struct.unpack('<I', header[40:44])[0]
    if version != 1 or page != 4096:
        raise SystemExit(f'boot.img is v{version} page {page}; dipper expects v1 page 4096')
    if 4096 + kernel_bytes + ramdisk_bytes > expanded:
        raise SystemExit('boot.img header claims more payload than the file holds')
    fit('boot', expanded, have)
    return {'kernel_bytes': kernel_bytes, 'ramdisk_bytes': ramdisk_bytes,
            'header_version': version, 'page_bytes': page}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--donor', type=Path, required=True,
                        help='Unpacked donor package root; images are read from extracted/base')
    parser.add_argument('--boot', type=Path, required=True,
                        help='boot.img built from the 4.9 kernel tree for dipper')
    parser.add_argument('--payload', type=Path, required=True,
                        help='Payload tree with compat/ and optional hmos/{sys_prod,preload}')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--layout', type=Path, default=LAYOUT_FILE)
    parser.add_argument('--mke2fs', default='mke2fs')
    for name in ('system', 'vendor', 'cust'):
        parser.add_argument(f'--{name}-bytes', type=int, default=None,
                            help=f'Device capacity after repartitioning {name}')
    args = parser.parse_args()

    _, partitions = device_layout(args.layout)
    assert_static_layout(partitions)
    images = args.output / 'images'
    if images.exists() and any(images.iterdir()):
        raise SystemExit(f'{images} is not empty; do not overwrite a release')
    images.mkdir(parents=True, exist_ok=True)
    reports = args.output / 'reports'
    reports.mkdir(exist_ok=True)
    args.work.mkdir(parents=True, exist_ok=True)

    report = {
        'kind': 'dipper-static-image-build',
        'device_flashed': False,
        'partition_model': 'single-slot static',
        'artifacts': [],
        'slot_suffix_used': False,
    }
    # boot 先验：头部不对就没必要再拷几个 GiB 的 system/vendor
    have = capacity(partitions, 'boot', None)
    report['boot'] = check_boot(args.boot, have)
    print('boot: ' + json.dumps(report['boot']), flush=True)
    shutil.copyfile(args.boot, images / 'boot.img')
    boot_bytes = (images / 'boot.img').stat().st_size
    report['artifacts'].append({
        'path': 'images/boot.img', 'bytes': boot_bytes,
        'sha256': sha256(images / 'boot.img'), 'expanded_bytes': boot_bytes,
        'format': 'raw-android-boot', 'capacity_bytes': have})

    for name in ('system', 'vendor'):
        source = args.donor / 'extracted/base' / f'{name}.img'
        if not source.is_file():
            raise SystemExit(f'missing donor image {source}')
        have = capacity(partitions, name, getattr(args, f'{name}_bytes'))
        record = copy_checked(source, images / f'{name}.img', name, have)
        print(f'{name}: {record["expanded_bytes"]} / {have} bytes', flush=True)
        report['artifacts'].append(record)

    have = capacity(partitions, 'cust', args.cust_bytes)
    record = build_cust(args.payload, images / 'cust.img', have, args.mke2fs, args.work)
    print(f'cust: {record["bytes"]} bytes, staged={record["staged_partitions"]}', flush=True)
    report['artifacts'].append(record)

    report['build_unix_time'] = int(time.time())
    report['verified_after_write'] = all(
        sha256(args.output / a['path']) == a['sha256'] for a in report['artifacts'])
    if not report['verified_after_write']:
        raise SystemExit('artifact readback mismatch')
    (reports / 'image-build.json').write_text(json.dumps(report, indent=2) + '\n',
                                             encoding='utf-8')
    print('Four images built and verified; no device was flashed.', flush=True)


if __name__ == '__main__':
    main()

"""Exercise userdata mounting on disposable loop images in a private namespace.

Run as root on Linux/WSL: python3 tests/test_userdata_mount.py
Only the sysfs partition identity is a fixture. Filesystem detection, UUIDs,
mounts, permissions and data preservation use real kernel/filesystem operations.
No physical phone or host data partition is accessed.
"""
from pathlib import Path
from contextlib import contextmanager
import json
import os
import shutil
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'runtime/compat/graphics/init-userdata-ext4.sh'


def run(*args, check=True):
    result = subprocess.run(args, text=True, capture_output=True)
    if check and result.returncode:
        raise RuntimeError(f'{args}: {result.stdout}\n{result.stderr}')
    return result


@contextmanager
def temporary_userdata():
    with tempfile.TemporaryDirectory(prefix='dipper-userdata-') as directory:
        work = Path(directory)
        image = work / 'userdata.ext4'
        with image.open('wb') as stream:
            stream.truncate(64 * 1024 * 1024)
        device = run('losetup', '--find', '--show', str(image)).stdout.strip()
        try:
            yield work, device
        finally:
            run('losetup', '--detach', device)


def inside():
    results = []
    with temporary_userdata() as (work, device):
        target = work / 'root/data'
        other = work / 'elsewhere'
        sysblock = work / 'sys' / Path(device).name
        sysblock.mkdir(parents=True)
        (sysblock / 'uevent').write_text('PARTNAME=userdata\n')
        (sysblock / 'size').write_text('131072\n')
        dev = os.stat(device).st_rdev
        (sysblock / 'dev').write_text(f'{os.major(dev)}:{os.minor(dev)}\n')
        # Forward BusyBox applets to the host tools; mount and blkid stay real.
        dispatcher = work / 'applets'
        dispatcher.write_text('#!/bin/sh\nexec "$@"\n')
        dispatcher.chmod(0o755)
        root = work / 'root'
        (root / 'dev/block/by-name').mkdir(parents=True)
        target.mkdir()
        other.mkdir()
        content = SCRIPT.read_text(encoding='utf-8')
        content = content.replace('B=/busybox\n', f'B={dispatcher}\n')
        content = content.replace('R=/hmos-root\n', f'R={root}\n')
        content = content.replace('U=/dev/block/by-name/userdata\n', f'U={device}\n')
        content = content.replace('/sys/class/block/', f'{work}/sys/')
        test_script = work / 'mount.sh'
        test_script.write_text(content)
        # Compile the same C storage check used by persistent logging, natively.
        harness = work / 'mount-check.c'
        harness.write_text('#define main bootstrap_main\n#include "' +
            str(ROOT / 'compat/bootstrap/hmos_probe_v121.c') +
            '"\n#undef main\nint main(int argc,char **argv) {'
            'return argc==3 && userdata_is_mounted(argv[1],argv[2]) ? 0 : 1;}\n')
        mount_check = work / 'mount-check'
        # AArch64-only tracing leaves unused helpers in this host-CPU harness.
        run('gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
            str(harness), '-o', str(mount_check))

        def placeholder():
            for name in ['userdata', 'by-name/userdata']:
                (root / 'dev/block' / name).unlink(missing_ok=True)
            run('mount', '-t', 'tmpfs', '-o', 'size=4m', 'tmpfs', str(target))

        def unmount():
            run('umount', str(target), check=False)

        def execute(expected, label):
            result = run('sh', str(test_script), check=False)
            assert result.returncode == expected, (label, result.stdout, result.stderr)
            results.append(label)
            return result

        def format_ext4(identity):
            run('mkfs.ext4', '-F', '-q', '-U', identity, str(device))

        try:
            placeholder()
            result = execute(61, 'blank userdata rejected with an actionable error')
            assert 'no filesystem detected' in result.stderr
            assert run(str(mount_check), device, str(target), check=False).returncode == 1
            # A partition with a valid filesystem must still be named userdata.
            format_ext4(str(uuid.uuid4()))
            (sysblock / 'uevent').write_text('PARTNAME=system\n')
            execute(1, 'wrong partition name rejected')
            (sysblock / 'uevent').write_text('PARTNAME=userdata\n')
            for index in range(2):
                identity = str(uuid.uuid4())
                format_ext4(identity)
                execute(0, f'fresh ext4 with random UUID {index + 1} accepted without marker')
                assert not (target / '.dipper-userdata-layout').exists()
                assert os.stat(target).st_dev == os.stat(device).st_rdev
                run(str(mount_check), device, str(target))
                assert run(str(mount_check), '/dev/null', str(target), check=False).returncode == 1
                cache = target / 'app/el2/0/base/com.ohos.settingsdata/cache'
                assert cache.is_dir() and cache.stat().st_uid == 20001
                sentinel = target / 'existing-data'
                sentinel.write_text('preserve this file\n')
                # Legacy marker contents are deliberately irrelevant.
                (target / '.dipper-userdata-layout').write_text('old or unrelated marker\n')
                execute(60, 'already mounted userdata rejected')
                unmount()
                placeholder()
                execute(0, 'existing files and legacy marker survive next mount')
                assert sentinel.read_text() == 'preserve this file\n'
                assert (target / '.dipper-userdata-layout').read_text() == 'old or unrelated marker\n'
                assert identity in run('blkid', str(device)).stdout
                unmount()
                placeholder()
            # Legacy installations also remain usable without their marker.
            format_ext4('57275cef-3a0e-427c-a22e-66ca7e80b893')
            execute(0, 'previous UUID accepted without marker')
            unmount()
            placeholder()
            run('mount', '-t', 'ext4', str(device), str(other))
            try:
                execute(60, 'userdata mounted elsewhere rejected')
            finally:
                run('umount', str(other))
            run('mkfs.ext2', '-F', '-q', str(device))
            result = execute(61, 'non-ext4 filesystem rejected')
            assert 'must be ext4' in result.stderr
            assert 'TYPE="ext2"' in run('blkid', str(device)).stdout
        finally:
            unmount()
            run('umount', str(other), check=False)
    print(json.dumps({'passed': len(results), 'cases': results,
                      'uses_disposable_images_only': True}, indent=2))


if __name__ == '__main__':
    if os.geteuid() != 0:
        raise SystemExit('Run as root in Linux/WSL; tests use a private mount namespace.')
    if '--inside' in sys.argv:
        inside()
    else:
        required = ['unshare', 'losetup', 'mount', 'umount', 'mkfs.ext4', 'mkfs.ext2', 'blkid', 'gcc']
        for program in required:
            if not shutil.which(program):
                raise SystemExit('Missing test dependency: ' + program)
        subprocess.run(['unshare', '--mount', '--propagation', 'private',
                        sys.executable, str(Path(__file__).resolve()), '--inside'], check=True)

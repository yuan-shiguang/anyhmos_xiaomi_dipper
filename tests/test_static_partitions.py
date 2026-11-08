"""Exercise the dipper static-partition mounts on disposable loop images.

Run as root on Linux/WSL: python3 tests/test_static_partitions.py

米 8 没有 super：system/vendor 是真实静态分区，兼容层载荷和 sys_prod/preload 放在
cust 上。这里用真实 ext4 镜像和真实 mount 调用验证那段 C 代码，只把分区名和挂载点
换成临时目录。不接触任何真机分区，也不访问宿主机的 /hmos 或 /cust。
"""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'compat/bootstrap/hmos_probe_v121.c'


def run(*args, check=True):
    result = subprocess.run(args, text=True, capture_output=True)
    if check and result.returncode:
        raise RuntimeError(f'{args}: {result.stdout}\n{result.stderr}')
    return result


def make_image(work, name, megabytes, content=None):
    """Create a real ext4 image on a loop device and fill it from content."""
    image = work / f'{name}.ext4'
    with image.open('wb') as stream:
        stream.truncate(megabytes * 1024 * 1024)
    device = run('losetup', '--find', '--show', str(image)).stdout.strip()
    run('mkfs.ext4', '-F', '-q', str(device))
    if content:
        target = work / f'{name}.content'
        target.mkdir()
        run('mount', str(device), str(target))
        try:
            for entry in sorted(Path(content).iterdir()):
                subprocess.run(['cp', '-a', str(entry), str(target) + '/'], check=True)
        finally:
            run('umount', str(target))
    return device


def umount_tree(prefix):
    """Detach every mount under prefix, deepest first."""
    table = Path('/proc/self/mountinfo').read_text()
    points = []
    for line in table.splitlines():
        fields = line.split()
        point = fields[4]
        if point == str(prefix) or point.startswith(str(prefix) + '/'):
            points.append(point)
    for point in sorted(points, key=len, reverse=True):
        run('umount', '-l', point, check=False)


def build_harness(work):
    harness = work / 'static-mount-harness.c'
    harness.write_text(
        '#define main bootstrap_main\n'
        f'#include "{SOURCE}"\n'
        '#undef main\n'
        'int main(int argc,char **argv) {\n'
        '    consolefd=1;\n'
        '    if(argc==4&&!strcmp(argv[1],"static"))\n'
        '        return static_partitions_at(argv[2],argv[3])==0?0:1;\n'
        '    if(argc==6&&!strcmp(argv[1],"payload"))\n'
        '        return cust_payload_at(argv[2],argv[3],argv[4],argv[5])==0?0:1;\n'
        '    return 2;\n'
        '}\n'
    )
    binary = work / 'static-mount-harness'
    # AArch64-only tracing leaves unused helpers in this host-CPU harness.
    run('gcc', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        str(harness), '-o', str(binary))
    return binary


def write(path, text, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(mode)


def system_content(target):
    write(target / 'etc/init', 'harmony init\n', 0o755)
    write(target / 'lib64/libace.z.so', 'system lib\n')


def vendor_content(target):
    write(target / 'lib64/libdisplay_composer_service.z.so', 'vendor lib\n')
    write(target / 'firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini', 'wlan cfg\n')


def cust_content(target):
    # /cust/compat 就是载荷根：启动脚本用 /compat/bin/init 起第二阶段
    write(target / 'compat/bin/init', 'harmony init copy\n', 0o755)
    write(target / 'compat/lib64/libdipper_audio_vdi.so', 'audio vdi\n')
    write(target / 'compat/graphics/apply-touch.sh', 'echo touch\n', 0o755)
    write(target / 'compat/busybox', 'busybox stub\n', 0o755)
    write(target / 'compat/hmos-init-debug.cfg', '{"init": []}\n')
    write(target / 'hmos/sys_prod/etc/param/hmos.para', 'sys_prod\n')
    write(target / 'hmos/preload/app/com.ohos.demo/a.abc', 'preload\n')


def inside():
    results = []
    with tempfile.TemporaryDirectory(prefix='dipper-static-',
                                      ignore_cleanup_errors=True) as directory:
        work = Path(directory)
        for name, content in [('system', system_content),
                              ('vendor', vendor_content),
                              ('cust', cust_content)]:
            seed = work / f'{name}.source'
            seed.mkdir()
            content(seed)
        harness = build_harness(work)
        devroot = work / 'dev-by-name'
        devroot.mkdir()
        mntroot = work / 'mnt'
        devices = {}
        try:
            for name, megabytes in [('system', 8), ('vendor', 8), ('cust', 16)]:
                devices[name] = make_image(work, name, megabytes, work / f'{name}.source')
                os.symlink(devices[name], devroot / name)

            # 1. 两个静态分区按 by-name 挂成只读
            result = run(str(harness), 'static', str(devroot), str(mntroot), check=False)
            assert result.returncode == 0, result.stdout + result.stderr
            assert (mntroot / 'system/etc/init').read_text() == 'harmony init\n'
            assert (mntroot / 'vendor/lib64/libdisplay_composer_service.z.so').is_file()
            for name in ('system', 'vendor'):
                try:
                    (mntroot / name / 'write-probe').write_text('x')
                except OSError as error:
                    assert error.errno == 30, (name, error)  # EROFS
                else:
                    raise AssertionError(f'{name} must be mounted read-only')
            results.append('system and vendor mount read-only by partition name')
            umount_tree(mntroot)

            # 2. 缺 vendor 时必须失败，而不是带着半个布局继续
            partial = work / 'dev-partial'
            partial.mkdir()
            os.symlink(devices['system'], partial / 'system')
            result = run(str(harness), 'static', str(partial), str(work / 'mnt-partial'),
                         check=False)
            assert result.returncode == 1, 'a missing vendor partition must fail the layout'
            assert 'FAIL static partition vendor missing' in result.stdout
            results.append('missing vendor partition fails instead of half-booting')
            umount_tree(work / 'mnt-partial')

            # 3. cust 挂成可写，载荷绑到目标根，sys_prod/preload 绑到暂存根
            cust_mnt = work / 'cust-mnt'
            bound = work / 'root'
            staged = work / 'hmos'
            bound.mkdir()
            result = run(str(harness), 'payload', devices['cust'], str(cust_mnt),
                         str(bound), str(staged), check=False)
            assert result.returncode == 0, result.stdout + result.stderr
            # 载荷根整体绑到 /compat，第二阶段从这里启动
            assert (bound / 'compat/bin/init').is_file()
            assert (bound / 'compat/lib64/libdipper_audio_vdi.so').is_file()
            # 脚本按绝对路径引用的几项单独再绑一次
            assert (bound / 'busybox').read_text() == 'busybox stub\n'
            assert (bound / 'graphics/apply-touch.sh').is_file()
            assert (bound / 'hmos-init-debug.cfg').is_file()
            # 不能整目录展开：initramfs 自己的 /bin、/lib64 必须保留
            assert not (bound / 'lib64').exists(), 'payload must not shadow initramfs /lib64'
            assert not (bound / 'bin').exists(), 'payload must not shadow initramfs /bin'
            assert (staged / 'sys_prod/etc/param/hmos.para').read_text() == 'sys_prod\n'
            assert (staged / 'preload/app/com.ohos.demo/a.abc').is_file()
            try:
                (staged / 'sys_prod/write-probe').write_text('x')
            except OSError as error:
                assert error.errno == 30, error
            else:
                raise AssertionError('staged sys_prod must be bound read-only')
            # 载荷脚本要 chmod 自己的文件，所以 cust 本身必须是可写的
            (cust_mnt / 'compat/write-probe').write_text('rw\n')
            results.append('payload binds /compat plus absolute paths, staged parts read-only')

            # 3b. 载荷缺 bin/init 时不能假装启动成功
            broken_source = work / 'broken.source'
            write(broken_source / 'compat/graphics/x.sh', 'echo x\n')
            devices['cust-broken'] = make_image(work, 'cust-broken', 4, broken_source)
            result = run(str(harness), 'payload', devices['cust-broken'],
                         str(work / 'cust-broken-mnt'), str(bound), str(staged), check=False)
            assert result.returncode == 1
            assert 'has no bin/init' in result.stdout
            results.append('payload without bin/init is refused')

            # 4. 缺载荷目录时明确失败
            empty = work / 'cust-empty.ext4'
            with empty.open('wb') as stream:
                stream.truncate(4 * 1024 * 1024)
            device = run('losetup', '--find', '--show', str(empty)).stdout.strip()
            devices['cust-empty'] = device
            run('mkfs.ext4', '-F', '-q', device)
            result = run(str(harness), 'payload', device, str(work / 'cust-empty-mnt'),
                         str(bound), str(staged), check=False)
            assert result.returncode == 1
            assert 'compatibility payload missing' in result.stdout
            results.append('cust without /compat payload is reported, not ignored')
        finally:
            umount_tree(work)
            for device in devices.values():
                run('losetup', '--detach', device, check=False)
    print(json.dumps({'passed': len(results), 'cases': results,
                      'uses_disposable_images_only': True}, indent=2))


if __name__ == '__main__':
    if os.geteuid() != 0:
        raise SystemExit('Run as root in Linux/WSL; tests use a private mount namespace.')
    if '--inside' in sys.argv:
        inside()
    else:
        required = ['unshare', 'losetup', 'mount', 'umount', 'mkfs.ext4', 'gcc', 'cp']
        for program in required:
            if not shutil.which(program):
                raise SystemExit('Missing test dependency: ' + program)
        subprocess.run(['unshare', '--mount', '--propagation', 'private',
                        sys.executable, str(Path(__file__).resolve()), '--inside'],
                       check=True)

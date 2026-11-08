"""Check the sdm845 tinyalsa audio backend keeps a correct kernel ABI and wiring.

The backend talks to /dev/snd with hand-written ALSA UAPI structures, so a wrong
size or ioctl number silently misconfigures the PCM. These tests re-derive the
values from the kernel uapi headers when a kernel tree is available, and compile
the source with the host compiler so its own _Static_assert guards are checked.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'compat/audio/tinyalsa_server.c'
START = ROOT / 'runtime/compat/graphics/start-audio-v116.sh'
APPLY = ROOT / 'runtime/compat/graphics/apply-audio-v116.sh'

PROBE = r'''
#include <sys/ioctl.h>
#include <stdio.h>
#include <stddef.h>
#include <sound/asound.h>
int main(void)
{
    printf("hw_params %zu\n", sizeof(struct snd_pcm_hw_params));
    printf("sw_params %zu\n", sizeof(struct snd_pcm_sw_params));
    printf("xferi %zu\n", sizeof(struct snd_xferi));
    printf("mask %zu\n", sizeof(struct snd_mask));
    printf("interval %zu\n", sizeof(struct snd_interval));
    printf("elem_id %zu\n", sizeof(struct snd_ctl_elem_id));
    printf("elem_list %zu\n", sizeof(struct snd_ctl_elem_list));
    printf("elem_value %zu\n", sizeof(struct snd_ctl_elem_value));
    printf("value_offset %zu\n", offsetof(struct snd_ctl_elem_value, value));
    printf("hw_params_ioctl %lu\n", (unsigned long)SNDRV_PCM_IOCTL_HW_PARAMS);
    printf("hw_refine_ioctl %lu\n", (unsigned long)SNDRV_PCM_IOCTL_HW_REFINE);
    printf("sw_params_ioctl %lu\n", (unsigned long)SNDRV_PCM_IOCTL_SW_PARAMS);
    printf("writei_ioctl %lu\n", (unsigned long)SNDRV_PCM_IOCTL_WRITEI_FRAMES);
    printf("readi_ioctl %lu\n", (unsigned long)SNDRV_PCM_IOCTL_READI_FRAMES);
    printf("prepare_ioctl %lu\n", (unsigned long)SNDRV_PCM_IOCTL_PREPARE);
    printf("start_ioctl %lu\n", (unsigned long)SNDRV_PCM_IOCTL_START);
    printf("drop_ioctl %lu\n", (unsigned long)SNDRV_PCM_IOCTL_DROP);
    printf("drain_ioctl %lu\n", (unsigned long)SNDRV_PCM_IOCTL_DRAIN);
    printf("pause_ioctl %lu\n", (unsigned long)SNDRV_PCM_IOCTL_PAUSE);
    printf("elem_list_ioctl %lu\n", (unsigned long)SNDRV_CTL_IOCTL_ELEM_LIST);
    printf("elem_write_ioctl %lu\n", (unsigned long)SNDRV_CTL_IOCTL_ELEM_WRITE);
    printf("iface_mixer %d\n", SNDRV_CTL_ELEM_IFACE_MIXER);
    printf("fmt_s16_le %d\n", SNDRV_PCM_FORMAT_S16_LE);
    printf("access_rw %d\n", SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    return 0;
}
'''


def host_cc():
    for name in ('cc', 'gcc', 'clang'):
        path = shutil.which(name)
        if path:
            return path
    return None


def kernel_dir():
    candidates = [
        os.environ.get('DIPPER_KERNEL_DIR'),
        '/home/yuanshiguang/android_kernel_xiaomi_sdm845',
        '/root/kernel_xiaomi_sdm845',
    ]
    for candidate in candidates:
        if not candidate:
            continue
        root = Path(candidate)
        if (root / 'include/uapi/sound/asound.h').is_file():
            return root
    return None


def asserted(source, pattern):
    match = re.search(pattern, source)
    if not match:
        raise AssertionError('missing assert: ' + pattern)
    return int(match.group(1), 0)


class TinyAlsaBackendTests(unittest.TestCase):
    def test_host_compile_checks_embedded_abi_asserts(self):
        cc = host_cc()
        if not cc:
            self.skipTest('no host C compiler')
        result = subprocess.run(
            [cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
             '-I' + str(SOURCE.parent), '-fsyntax-only', str(SOURCE)],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_structure_sizes_and_ioctl_numbers_match_kernel_uapi(self):
        kernel = kernel_dir()
        cc = host_cc()
        if not kernel or not cc:
            self.skipTest('kernel uapi headers or host compiler unavailable')
        with tempfile.TemporaryDirectory() as work:
            work = Path(work)
            probe = work / 'probe.c'
            probe.write_text(PROBE)
            binary = work / 'probe'
            build = subprocess.run(
                [cc, '-D__EXPORTED_HEADERS__',
                 '-I' + str(kernel / 'include/uapi'), '-I' + str(kernel / 'include'),
                 '-o', str(binary), str(probe)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
        actual = {}
        for line in run.stdout.splitlines():
            key, value = line.split()
            actual[key] = int(value)

        source = SOURCE.read_text(encoding='utf-8')
        # 结构体尺寸：与 _Static_assert 中的字面值逐一对应
        for name, pattern in [
            ('hw_params', r'sizeof\(struct dipper_pcm_hw_params\) == (\d+)'),
            ('sw_params', r'sizeof\(struct dipper_pcm_sw_params\) == (\d+)'),
            ('xferi', r'sizeof\(struct dipper_snd_xferi\) == (\d+)'),
            ('mask', r'sizeof\(struct dipper_snd_mask\) == (\d+)'),
            ('interval', r'sizeof\(struct dipper_snd_interval\) == (\d+)'),
            ('elem_id', r'sizeof\(struct dipper_ctl_elem_id\) == (\d+)'),
            ('elem_list', r'sizeof\(struct dipper_ctl_elem_list\) == (\d+)'),
            ('elem_value', r'sizeof\(struct dipper_ctl_elem_value\) == (\d+)'),
            ('value_offset', r'offsetof\(struct dipper_ctl_elem_value, value\) == (\d+)'),
        ]:
            self.assertEqual(asserted(source, pattern), actual[name], name)

        # ioctl 号：源文件里绑定的是十进制字面值
        for name, pattern in [
            ('hw_params_ioctl', r'DIPPER_IOCTL_HW_PARAMS == (0x[0-9a-fA-F]+)UL'),
            ('hw_refine_ioctl', r'DIPPER_IOCTL_HW_REFINE == (0x[0-9a-fA-F]+)UL'),
            ('sw_params_ioctl', r'DIPPER_IOCTL_SW_PARAMS == (0x[0-9a-fA-F]+)UL'),
            ('writei_ioctl', r'DIPPER_IOCTL_WRITEI == (0x[0-9a-fA-F]+)UL'),
            ('readi_ioctl', r'DIPPER_IOCTL_READI == (0x[0-9a-fA-F]+)UL'),
            ('prepare_ioctl', r'DIPPER_IOCTL_PREPARE == (0x[0-9a-fA-F]+)UL'),
            ('start_ioctl', r'DIPPER_IOCTL_START == (0x[0-9a-fA-F]+)UL'),
            ('drop_ioctl', r'DIPPER_IOCTL_DROP == (0x[0-9a-fA-F]+)UL'),
            ('drain_ioctl', r'DIPPER_IOCTL_DRAIN == (0x[0-9a-fA-F]+)UL'),
            ('pause_ioctl', r'DIPPER_IOCTL_PAUSE == (0x[0-9a-fA-F]+)UL'),
        ]:
            self.assertEqual(asserted(source, pattern), actual[name], name)

        # 混音器控制接口：elem_list 的 pids 指针与 elem_write 的尺寸
        self.assertIn('_IOWR(\'U\', 0x10, struct dipper_ctl_elem_list)', source)
        self.assertIn('_IOWR(\'U\', 0x13, struct dipper_ctl_elem_value)', source)
        self.assertEqual(actual['elem_list_ioctl'] & 0x0000ffff, 0x5510)
        self.assertEqual(actual['elem_write_ioctl'] & 0x0000ffff, 0x5513)
        self.assertEqual(actual['iface_mixer'], 2)

        # 采样格式与访问方式必须和 hw_params 里写入的常量一致
        self.assertEqual(actual['fmt_s16_le'], 2)
        self.assertEqual(actual['access_rw'], 3)
        self.assertIn('#define DIPPER_PCM_FORMAT_S16_LE 2u', source)
        self.assertIn('#define DIPPER_PCM_ACCESS_RW_INTERLEAVED 3u', source)

    def test_startup_script_selects_backend_without_losing_readiness_gate(self):
        script = START.read_text(encoding='utf-8')
        self.assertIn('AUDIO_CARD_CHECKED=1', script)
        self.assertIn('AUDIO_BACKEND=pal-agm', script)
        self.assertIn('AUDIO_BACKEND=tinyalsa', script)
        self.assertIn('AUDIO_BACKEND_ABSENT_NO_TINYALSA=1', script)
        self.assertNotIn('AUDIO_PAL_BACKEND_UNAVAILABLE_NO_AGM', script)
        # 两个后端共用同一份就绪判定，避免出现第二套等待逻辑
        self.assertEqual(script.count("grep -q '^PAL_SERVER_READY=1 '"), 1)
        # 没有 AGM 时不再无条件退出
        self.assertNotIn('NO_AGM=1\n        exit 0', script)
        # /dev/snd 只在整个目录为空时绑定，不假定声卡编号
        self.assertIn('mount --bind /dev/snd "$R/dev/snd"', script)
        self.assertNotIn('pcmC0D0p', script)

    def test_payload_stages_backend_as_executable(self):
        apply = APPLY.read_text(encoding='utf-8')
        self.assertIn('dipper-tinyalsa-server', apply)
        self.assertIn('chmod 0755 "$R/compat/bin/dipper-tinyalsa-server"', apply)


if __name__ == '__main__':
    unittest.main()

/* SPDX-License-Identifier: MIT
 * 米 8（dipper）tinyalsa PCM 后端。
 *
 * 为什么需要它：华为 PAL/AGM 只在 sm8550 级平台上存在，sdm845 没有 /dev/agm，
 * 原 libdipper_pal_server.so 依赖 libar-pal.so + libagm.so，在米 8 上无法工作。
 * 本后端复用同一套 dipper.pal.pcm.v1 线协议（protocol.h），把请求直接落到内核
 * ALSA PCM 节点（/dev/snd/pcmCxDyp），因此上层的 libdipper_audio_vdi.so 不需要改动。
 *
 * 只使用内核 UAPI 定义的 ioctl 与结构体，不依赖 alsa-lib，也不依赖任何厂商库；
 * 结构体尺寸与 ioctl 号在编译期用 _Static_assert 绑定到 4.9 内核的核对值。
 *
 * 声卡与 PCM 设备在运行时从 /proc/asound 解析，不写死编号：米 8 的
 * sdm845-tavil-snd-card 上前端 DAI 的 PCM 编号随内核配置变化，写死 pcmC0D0p
 * 一旦顺序不同就会静默写到错误的 substream。
 */
#define _GNU_SOURCE
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* ---------------------------------------------------------------- ALSA UAPI */

#define DIPPER_SND_MASK_MAX 256u

struct dipper_snd_mask {
    uint32_t bits[DIPPER_SND_MASK_MAX / 32u];
};

struct dipper_snd_interval {
    uint32_t min, max;
    uint32_t openmin : 1, openmax : 1, integer : 1, empty : 1;
};

enum {
    DIPPER_PARAM_ACCESS = 0,
    DIPPER_PARAM_FORMAT = 1,
    DIPPER_PARAM_SUBFORMAT = 2,
    DIPPER_PARAM_SAMPLE_BITS = 8,
    DIPPER_PARAM_FRAME_BITS = 9,
    DIPPER_PARAM_CHANNELS = 10,
    DIPPER_PARAM_RATE = 11,
    DIPPER_PARAM_PERIOD_TIME = 12,
    DIPPER_PARAM_PERIOD_SIZE = 13,
    DIPPER_PARAM_PERIOD_BYTES = 14,
    DIPPER_PARAM_PERIODS = 15,
    DIPPER_PARAM_BUFFER_TIME = 16,
    DIPPER_PARAM_BUFFER_SIZE = 17,
    DIPPER_PARAM_BUFFER_BYTES = 18,
    DIPPER_PARAM_TICK_TIME = 19
};

#define DIPPER_PARAM_LAST_MASK DIPPER_PARAM_SUBFORMAT
#define DIPPER_PARAM_FIRST_INTERVAL DIPPER_PARAM_SAMPLE_BITS
#define DIPPER_PARAM_LAST_INTERVAL DIPPER_PARAM_TICK_TIME
#define DIPPER_PARAM_MASKS (DIPPER_PARAM_LAST_MASK + 1)
#define DIPPER_PARAM_INTERVALS \
    (DIPPER_PARAM_LAST_INTERVAL - DIPPER_PARAM_FIRST_INTERVAL + 1)
#define DIPPER_IV(param) ((param) - DIPPER_PARAM_FIRST_INTERVAL)

/* 与 include/uapi/sound/asound.h 逐字段一致；下面的 _Static_assert 兜住尺寸 */
struct dipper_pcm_hw_params {
    uint32_t flags;
    struct dipper_snd_mask masks[DIPPER_PARAM_MASKS];
    struct dipper_snd_mask mres[5];
    struct dipper_snd_interval intervals[DIPPER_PARAM_INTERVALS];
    struct dipper_snd_interval ires[9];
    uint32_t rmask, cmask, info, msbits, rate_num, rate_den;
    unsigned long fifo_size;
    unsigned char reserved[64];
};

struct dipper_pcm_sw_params {
    int tstamp_mode;
    uint32_t period_step, sleep_min;
    unsigned long avail_min, xfer_align, start_threshold, stop_threshold;
    unsigned long silence_threshold, silence_size, boundary;
    unsigned char reserved[64];
};

struct dipper_snd_xferi {
    void *buf;
    unsigned long frames;
    long result;
};

_Static_assert(sizeof(struct dipper_snd_mask) == 32, "snd_mask is 8 words");
_Static_assert(sizeof(struct dipper_snd_interval) == 12, "snd_interval is 3 words");
#if __SIZEOF_LONG__ == 8
_Static_assert(sizeof(struct dipper_pcm_hw_params) == 608, "hw_params ABI size");
_Static_assert(sizeof(struct dipper_pcm_sw_params) == 136, "sw_params ABI size");
_Static_assert(sizeof(struct dipper_snd_xferi) == 24, "snd_xferi ABI size");
#endif

#define DIPPER_IOCTL_PVERSION _IOR('A', 0x00, int)
#define DIPPER_IOCTL_HW_REFINE _IOWR('A', 0x10, struct dipper_pcm_hw_params)
#define DIPPER_IOCTL_HW_PARAMS _IOWR('A', 0x11, struct dipper_pcm_hw_params)
#define DIPPER_IOCTL_SW_PARAMS _IOWR('A', 0x13, struct dipper_pcm_sw_params)
#define DIPPER_IOCTL_PREPARE _IO('A', 0x40)
#define DIPPER_IOCTL_RESET _IO('A', 0x41)
#define DIPPER_IOCTL_START _IO('A', 0x42)
#define DIPPER_IOCTL_DROP _IO('A', 0x43)
#define DIPPER_IOCTL_DRAIN _IO('A', 0x44)
#define DIPPER_IOCTL_PAUSE _IOW('A', 0x45, int)
#define DIPPER_IOCTL_WRITEI _IOW('A', 0x50, struct dipper_snd_xferi)
#define DIPPER_IOCTL_READI _IOR('A', 0x51, struct dipper_snd_xferi)

#if __SIZEOF_LONG__ == 8
_Static_assert(DIPPER_IOCTL_PVERSION == 0x80044100UL, "pversion ioctl number");
_Static_assert(DIPPER_IOCTL_HW_REFINE == 0xc2604110UL, "hw_refine ioctl number");
_Static_assert(DIPPER_IOCTL_HW_PARAMS == 0xc2604111UL, "hw_params ioctl number");
_Static_assert(DIPPER_IOCTL_SW_PARAMS == 0xc0884113UL, "sw_params ioctl number");
_Static_assert(DIPPER_IOCTL_PREPARE == 0x4140UL, "prepare ioctl number");
_Static_assert(DIPPER_IOCTL_START == 0x4142UL, "start ioctl number");
_Static_assert(DIPPER_IOCTL_DROP == 0x4143UL, "drop ioctl number");
_Static_assert(DIPPER_IOCTL_DRAIN == 0x4144UL, "drain ioctl number");
_Static_assert(DIPPER_IOCTL_PAUSE == 0x40044145UL, "pause ioctl number");
_Static_assert(DIPPER_IOCTL_WRITEI == 0x40184150UL, "writei ioctl number");
_Static_assert(DIPPER_IOCTL_READI == 0x80184151UL, "readi ioctl number");
#endif

/* 混音器控制：4.9 起 elem_list 用 pids 指针回写，元素区紧跟 16 字节头部 */
struct dipper_ctl_elem_id {
    uint32_t numid, iface, device, subdevice;
    unsigned char name[44];
    uint32_t index;
};

struct dipper_ctl_elem_list {
    uint32_t offset, space, used, count;
    uint64_t pids;
    unsigned char reserved[50];
};

struct dipper_ctl_elem_value {
    struct dipper_ctl_elem_id id;
    uint32_t indirect;
    uint32_t pad;
    uint64_t value[128];
    unsigned char tail[128];
};

_Static_assert(sizeof(struct dipper_ctl_elem_id) == 64, "ctl_elem_id ABI size");
_Static_assert(sizeof(struct dipper_ctl_elem_list) == 80, "ctl_elem_list ABI size");
_Static_assert(sizeof(struct dipper_ctl_elem_value) == 1224, "ctl_elem_value ABI size");
_Static_assert(offsetof(struct dipper_ctl_elem_value, value) == 72, "ctl value offset");

#define DIPPER_CTL_IOCTL_ELEM_LIST _IOWR('U', 0x10, struct dipper_ctl_elem_list)
#define DIPPER_CTL_IOCTL_ELEM_WRITE _IOWR('U', 0x13, struct dipper_ctl_elem_value)
#define DIPPER_CTL_IFACE_MIXER 2u

#define DIPPER_PCM_FORMAT_S16_LE 2u
#define DIPPER_PCM_ACCESS_RW_INTERLEAVED 3u
#define DIPPER_PCM_SUBFORMAT_STD 0u

/* ------------------------------------------------------------------ 运行态 */

#define DIPPER_MAX_CLIENTS 8
#define DIPPER_MAX_CTL_VALUES 4

enum { DIPPER_PCM_CLOSED = 0, DIPPER_PCM_OPEN, DIPPER_PCM_RUNNING, DIPPER_PCM_PAUSED };

struct dipper_pcm {
    int fd;
    unsigned card, device;
    unsigned rate, channels;
    unsigned period_size, periods, buffer_size;
    int state;
    int xruns;
};

struct dipper_client {
    int fd;
    struct dipper_pcm out, in;
    uint64_t frames, capture_frames;
    uint64_t nonzero_samples;
    unsigned peak;
    int gain_q15;
};

static volatile sig_atomic_t stopping;
static unsigned opt_card = ~0u;
static int opt_playback = -1, opt_capture = -1;
static const char *opt_socket = DIPPER_PAL_SOCKET;
static const char *opt_route;
static unsigned opt_period = 960, opt_periods = 4;
static int opt_dump_mixer;
static int mic_muted;
static int route_applied;
static unsigned route_card = ~0u;

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void log_line(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof(b) - 2)
        n = (int)sizeof(b) - 2;
    b[n++] = '\n';
    ssize_t ignored = write(STDOUT_FILENO, b, (size_t)n);
    (void)ignored;
}

/* -------------------------------------------------------- /proc/asound 解析 */

/* 返回包含关键字的声卡编号；-1 表示没有找到 */
static int find_card(const char *needle)
{
    FILE *f = fopen("/proc/asound/cards", "re");
    if (!f)
        return -1;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ')
            p++;
        if (*p < '0' || *p > '9')
            continue;
        if (!strstr(p, needle))
            continue;
        found = atoi(p);
        break;
    }
    fclose(f);
    return found;
}

struct dipper_pcm_ref {
    unsigned card, device;
    char name[64];
};

/* 去掉 "MultiMedia1 (*) " 这类后缀，只留下 DAI 名 */
static void trim_pcm_name(char *name)
{
    size_t n = strlen(name);
    while (n && (name[n - 1] == ' ' || name[n - 1] == '\t'))
        name[--n] = 0;
    char *mark = strstr(name, " (*)");
    if (mark) {
        *mark = 0;
        n = strlen(name);
        while (n && name[n - 1] == ' ')
            name[--n] = 0;
    }
}

static int scan_pcm(unsigned card, int want_playback, struct dipper_pcm_ref *out)
{
    FILE *f = fopen("/proc/asound/pcm", "re");
    if (!f)
        return -ENODEV;
    char line[256];
    int best_rank = 1 << 30;
    int found = -ENOENT;
    while (fgets(line, sizeof(line), f)) {
        unsigned c = 0, d = 0;
        char name[64] = {0};
        if (sscanf(line, "%u-%u: %63[^:]", &c, &d, name) != 3)
            continue;
        if (c != card)
            continue;
        trim_pcm_name(name);
        if (!name[0])
            continue;
        int has = want_playback ? (strstr(line, "playback") != NULL)
                                : (strstr(line, "capture") != NULL);
        if (!has)
            continue;
        int rank = 100;
        if (!strcmp(name, "MultiMedia1"))
            rank = 1;
        else if (!strcmp(name, "LowLatency"))
            rank = 2;
        else if (!strcmp(name, "MultiMedia10"))
            rank = 3;
        else if (!strcmp(name, "MultiMedia2"))
            rank = 4;
        if (rank < best_rank) {
            best_rank = rank;
            out->card = c;
            out->device = d;
            snprintf(out->name, sizeof(out->name), "%s", name);
            found = 0;
        }
    }
    fclose(f);
    return found;
}

/* ------------------------------------------------------------- 混音器路由 */

static int ctl_open(unsigned card)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/snd/controlC%u", card);
    return open(path, O_RDWR | O_CLOEXEC);
}

/* 写一个混音器控件；values 对应控件的 count 个成员 */
static int ctl_write(int fd, const char *name, const long *values, unsigned count)
{
    struct dipper_ctl_elem_value v;
    if (count == 0 || count > DIPPER_MAX_CTL_VALUES)
        return -EINVAL;
    memset(&v, 0, sizeof(v));
    v.id.iface = DIPPER_CTL_IFACE_MIXER;
    snprintf((char *)v.id.name, sizeof(v.id.name), "%s", name);
    for (unsigned i = 0; i < count; i++)
        v.value[i] = (uint64_t)values[i];
    if (ioctl(fd, DIPPER_CTL_IOCTL_ELEM_WRITE, &v) < 0)
        return -errno;
    return 0;
}

/* 解析 "控件名 = 值 [值...]" 形式的路由表；返回成功写入的条数 */
static int apply_route_file(int fd, const char *path)
{
    FILE *f = fopen(path, "re");
    if (!f) {
        log_line("TINYALSA_ROUTE_FILE_ABSENT=%s", path);
        return 0;
    }
    char line[256];
    int applied = 0;
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *eq = strrchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *name = line;
        while (*name == ' ' || *name == '\t')
            name++;
        size_t n = strlen(name);
        while (n && (name[n - 1] == ' ' || name[n - 1] == '\t'))
            name[--n] = 0;
        if (!name[0])
            continue;
        long values[DIPPER_MAX_CTL_VALUES] = {0};
        unsigned count = 0;
        char *cursor = eq + 1;
        while (count < DIPPER_MAX_CTL_VALUES) {
            char *end = NULL;
            long value = strtol(cursor, &end, 0);
            if (end == cursor)
                break;
            values[count++] = value;
            cursor = end;
        }
        if (!count)
            continue;
        int rc = ctl_write(fd, name, values, count);
        log_line("TINYALSA_ROUTE name=%s values=%u rc=%d", name, count, rc);
        if (!rc)
            applied++;
    }
    fclose(f);
    return applied;
}

/* 内置路由：下行 FE→SLIMBUS_0_RX，来自 msm-pcm-routing-v2.c 的 DAPM 路由表。
 * ASoC 把 DAPM 混音器的每个输入暴露成 "控件名 输入名" 形式的 kcontrol，
 * 因此这里按实际选中的前端 DAI 名拼出控件名，而不是写死 MultiMedia1。 */
static void apply_default_route(int fd, const struct dipper_pcm_ref *out)
{
    char name[96];
    long one = 1;
    if (out && out->name[0]) {
        snprintf(name, sizeof(name), "SLIMBUS_0_RX Audio Mixer %s", out->name);
        int rc = ctl_write(fd, name, &one, 1);
        log_line("TINYALSA_ROUTE name=%s rc=%d", name, rc);
    }
    /* TAS2557 智能功放：解除静音并声明采样率；控件缺失不算错误 */
    long unmute = 0;
    int rc = ctl_write(fd, "SmartPA Mute", &unmute, 1);
    log_line("TINYALSA_ROUTE name=SmartPA Mute rc=%d", rc);
    long fs = 48000;
    rc = ctl_write(fd, "FS", &fs, 1);
    log_line("TINYALSA_ROUTE name=FS rc=%d", rc);
}

static void apply_route_once(unsigned card, const struct dipper_pcm_ref *out)
{
    if (route_applied && route_card == card)
        return;
    int fd = ctl_open(card);
    if (fd < 0) {
        log_line("TINYALSA_MIXER_ABSENT card=%u errno=%d", card, errno);
        return;
    }
    if (opt_route) {
        int n = apply_route_file(fd, opt_route);
        log_line("TINYALSA_ROUTE_FILE_APPLIED=%d", n);
    } else {
        apply_default_route(fd, out);
    }
    close(fd);
    route_applied = 1;
    route_card = card;
    log_line("TINYALSA_ROUTE_DONE card=%u", card);
}

static void dump_mixer(unsigned card)
{
    int fd = ctl_open(card);
    if (fd < 0) {
        log_line("TINYALSA_MIXER_ABSENT card=%u errno=%d", card, errno);
        return;
    }
    size_t bytes = sizeof(struct dipper_ctl_elem_list) +
                   sizeof(struct dipper_ctl_elem_id) * 512u;
    struct dipper_ctl_elem_list *list = calloc(1, bytes);
    if (!list) {
        close(fd);
        return;
    }
    struct dipper_ctl_elem_id *ids =
        (struct dipper_ctl_elem_id *)((unsigned char *)list +
                                      sizeof(struct dipper_ctl_elem_list));
    list->offset = 0;
    list->space = 512;
    list->pids = (uint64_t)(uintptr_t)ids;
    if (ioctl(fd, DIPPER_CTL_IOCTL_ELEM_LIST, list) < 0) {
        log_line("TINYALSA_MIXER_LIST_FAILED errno=%d", errno);
        free(list);
        close(fd);
        return;
    }
    log_line("TINYALSA_MIXER_CONTROLS=%u", list->used);
    for (unsigned i = 0; i < list->used; i++)
        log_line("TINYALSA_MIXER_CONTROL=%s", ids[i].name);
    free(list);
    close(fd);
}

/* ----------------------------------------------------------------- PCM 操作 */

static void mask_set(struct dipper_snd_mask *m, unsigned value)
{
    if (value < DIPPER_SND_MASK_MAX)
        m->bits[value / 32u] |= 1u << (value % 32u);
}

static void interval_exact(struct dipper_snd_interval *i, unsigned value)
{
    i->min = value;
    i->max = value;
    i->integer = 1;
    i->empty = 0;
}

static void interval_range(struct dipper_snd_interval *i, unsigned lo, unsigned hi)
{
    i->min = lo;
    i->max = hi;
    i->integer = 0;
    i->empty = 0;
}

static int pcm_configure(struct dipper_pcm *p, unsigned rate, unsigned channels)
{
    struct dipper_pcm_hw_params hw;
    memset(&hw, 0, sizeof(hw));
    mask_set(&hw.masks[DIPPER_PARAM_ACCESS], DIPPER_PCM_ACCESS_RW_INTERLEAVED);
    mask_set(&hw.masks[DIPPER_PARAM_FORMAT], DIPPER_PCM_FORMAT_S16_LE);
    mask_set(&hw.masks[DIPPER_PARAM_SUBFORMAT], DIPPER_PCM_SUBFORMAT_STD);
    interval_exact(&hw.intervals[DIPPER_IV(DIPPER_PARAM_SAMPLE_BITS)], 16);
    interval_exact(&hw.intervals[DIPPER_IV(DIPPER_PARAM_FRAME_BITS)], 16 * channels);
    interval_exact(&hw.intervals[DIPPER_IV(DIPPER_PARAM_CHANNELS)], channels);
    interval_exact(&hw.intervals[DIPPER_IV(DIPPER_PARAM_RATE)], rate);
    interval_exact(&hw.intervals[DIPPER_IV(DIPPER_PARAM_PERIOD_SIZE)], opt_period);
    interval_exact(&hw.intervals[DIPPER_IV(DIPPER_PARAM_PERIODS)], opt_periods);
    interval_range(&hw.intervals[DIPPER_IV(DIPPER_PARAM_PERIOD_TIME)], 0, ~0u);
    interval_range(&hw.intervals[DIPPER_IV(DIPPER_PARAM_PERIOD_BYTES)], 0, ~0u);
    interval_range(&hw.intervals[DIPPER_IV(DIPPER_PARAM_BUFFER_TIME)], 0, ~0u);
    interval_range(&hw.intervals[DIPPER_IV(DIPPER_PARAM_BUFFER_SIZE)], 0, ~0u);
    interval_range(&hw.intervals[DIPPER_IV(DIPPER_PARAM_BUFFER_BYTES)], 0, ~0u);
    interval_range(&hw.intervals[DIPPER_IV(DIPPER_PARAM_TICK_TIME)], 0, ~0u);
    hw.rmask = (1u << DIPPER_PARAM_ACCESS) | (1u << DIPPER_PARAM_FORMAT) |
               (1u << DIPPER_PARAM_SUBFORMAT) | (1u << DIPPER_PARAM_SAMPLE_BITS) |
               (1u << DIPPER_PARAM_FRAME_BITS) | (1u << DIPPER_PARAM_CHANNELS) |
               (1u << DIPPER_PARAM_RATE) | (1u << DIPPER_PARAM_PERIOD_SIZE) |
               (1u << DIPPER_PARAM_PERIODS);
    if (ioctl(p->fd, DIPPER_IOCTL_HW_REFINE, &hw) < 0) {
        /* 精确 20 ms 周期不被接受时退化为区间协商，仍以实际值继续 */
        interval_range(&hw.intervals[DIPPER_IV(DIPPER_PARAM_PERIOD_SIZE)], 240, 4800);
        interval_range(&hw.intervals[DIPPER_IV(DIPPER_PARAM_PERIODS)], 2, 8);
        hw.rmask &= ~((1u << DIPPER_PARAM_PERIOD_SIZE) | (1u << DIPPER_PARAM_PERIODS));
        if (ioctl(p->fd, DIPPER_IOCTL_HW_REFINE, &hw) < 0) {
            log_line("TINYALSA_HW_REFINE_FAILED errno=%d", errno);
            return -errno;
        }
    }
    if (hw.intervals[DIPPER_IV(DIPPER_PARAM_RATE)].empty ||
        hw.intervals[DIPPER_IV(DIPPER_PARAM_CHANNELS)].empty ||
        hw.intervals[DIPPER_IV(DIPPER_PARAM_PERIOD_SIZE)].empty) {
        log_line("TINYALSA_HW_REFINE_EMPTY rate=%u channels=%u", rate, channels);
        return -EINVAL;
    }
    p->period_size = hw.intervals[DIPPER_IV(DIPPER_PARAM_PERIOD_SIZE)].min;
    p->periods = hw.intervals[DIPPER_IV(DIPPER_PARAM_PERIODS)].min;
    if (ioctl(p->fd, DIPPER_IOCTL_HW_PARAMS, &hw) < 0) {
        log_line("TINYALSA_HW_PARAMS_FAILED errno=%d", errno);
        return -errno;
    }
    p->rate = rate;
    p->channels = channels;
    p->buffer_size = p->period_size * (p->periods ? p->periods : 1);
    if (!p->buffer_size)
        p->buffer_size = p->period_size;
    log_line("TINYALSA_HW_PARAMS card=%u device=%u rate=%u channels=%u "
             "period=%u periods=%u buffer=%u",
             p->card, p->device, p->rate, p->channels, p->period_size, p->periods,
             p->buffer_size);
    return 0;
}

static int pcm_sw_configure(struct dipper_pcm *p)
{
    struct dipper_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode = 0;
    sw.period_step = 1;
    sw.avail_min = p->period_size;
    /* 由 start_threshold 触发启动：PAL 的 START 早于首个 WRITE，主动 START 会立刻欠载 */
    sw.start_threshold = p->period_size;
    sw.stop_threshold = p->buffer_size;
    sw.silence_threshold = 0;
    sw.silence_size = 0;
    sw.boundary = 0;
    if (ioctl(p->fd, DIPPER_IOCTL_SW_PARAMS, &sw) < 0) {
        log_line("TINYALSA_SW_PARAMS_FAILED errno=%d", errno);
        return -errno;
    }
    return 0;
}

static int pcm_open_dev(struct dipper_pcm *p, unsigned card, unsigned device,
                        unsigned rate, unsigned channels, int capture)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%uD%u%c", card, device,
             capture ? 'c' : 'p');
    p->fd = open(path, O_RDWR | O_CLOEXEC);
    if (p->fd < 0)
        return -errno;
    p->card = card;
    p->device = device;
    p->state = DIPPER_PCM_CLOSED;
    int rc = pcm_configure(p, rate, channels);
    if (!rc)
        rc = pcm_sw_configure(p);
    if (!rc && ioctl(p->fd, DIPPER_IOCTL_PREPARE) < 0)
        rc = -errno;
    if (rc) {
        close(p->fd);
        p->fd = -1;
        return rc;
    }
    p->state = DIPPER_PCM_OPEN;
    p->xruns = 0;
    return 0;
}

static void pcm_close_dev(struct dipper_pcm *p)
{
    if (p->fd < 0)
        return;
    if (p->state == DIPPER_PCM_RUNNING || p->state == DIPPER_PCM_PAUSED)
        (void)ioctl(p->fd, DIPPER_IOCTL_DROP);
    close(p->fd);
    p->fd = -1;
    p->state = DIPPER_PCM_CLOSED;
}

static int pcm_prepare(struct dipper_pcm *p)
{
    if (ioctl(p->fd, DIPPER_IOCTL_PREPARE) < 0)
        return -errno;
    p->state = DIPPER_PCM_OPEN;
    return 0;
}

static int pcm_transfer(struct dipper_pcm *p, void *buf, unsigned bytes, int capture)
{
    unsigned frame_bytes = p->channels * 2u;
    if (!frame_bytes || (bytes % frame_bytes))
        return -EINVAL;
    struct dipper_snd_xferi x = {.buf = buf, .frames = bytes / frame_bytes, .result = 0};
    for (int attempt = 0; attempt < 2; attempt++) {
        if (ioctl(p->fd, capture ? DIPPER_IOCTL_READI : DIPPER_IOCTL_WRITEI, &x) < 0) {
            int err = errno;
            if (err == EPIPE || err == ESTRPIPE) {
                p->xruns++;
                log_line("TINYALSA_XRUN card=%u device=%u capture=%d count=%d", p->card,
                         p->device, capture, p->xruns);
                if (pcm_prepare(p) < 0)
                    return -EIO;
                x.result = 0;
                continue;
            }
            return -err;
        }
        break;
    }
    if (x.result <= 0)
        return -EIO;
    if (p->state == DIPPER_PCM_OPEN)
        p->state = DIPPER_PCM_RUNNING;
    return (int)(x.result * frame_bytes);
}

/* -------------------------------------------------------------- 请求分派 */

static int open_playback(struct dipper_client *c, unsigned rate, unsigned channels)
{
    if (c->out.state != DIPPER_PCM_CLOSED || c->out.fd >= 0)
        return -EBUSY;
    if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2)
        return -EOPNOTSUPP;
    int card = opt_card == ~0u ? find_card("sdm845") : (int)opt_card;
    if (card < 0) {
        log_line("TINYALSA_CARD_ABSENT");
        return -ENODEV;
    }
    struct dipper_pcm_ref ref = {(unsigned)card, 0, {0}};
    if (opt_playback >= 0) {
        ref.device = (unsigned)opt_playback;
        snprintf(ref.name, sizeof(ref.name), "%s", "cli");
    } else if (scan_pcm((unsigned)card, 1, &ref) < 0) {
        log_line("TINYALSA_PLAYBACK_PCM_ABSENT card=%d", card);
        return -ENODEV;
    }
    int rc = pcm_open_dev(&c->out, ref.card, ref.device, rate, channels, 0);
    if (rc) {
        log_line("TINYALSA_PLAYBACK_OPEN_FAILED card=%u device=%u rc=%d", ref.card,
                 ref.device, rc);
        return rc;
    }
    log_line("TINYALSA_PLAYBACK_OPEN card=%u device=%u dai=%s", ref.card, ref.device,
             ref.name);
    apply_route_once(ref.card, &ref);
    c->frames = 0;
    c->nonzero_samples = 0;
    c->peak = 0;
    c->gain_q15 = 32768;
    return 0;
}

static int open_capture(struct dipper_client *c, unsigned rate, unsigned channels)
{
    if (c->in.state != DIPPER_PCM_CLOSED || c->in.fd >= 0)
        return -EBUSY;
    if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2)
        return -EOPNOTSUPP;
    int card = opt_card == ~0u ? find_card("sdm845") : (int)opt_card;
    if (card < 0) {
        log_line("TINYALSA_CARD_ABSENT");
        return -ENODEV;
    }
    struct dipper_pcm_ref ref = {(unsigned)card, 0, {0}};
    if (opt_capture >= 0) {
        ref.device = (unsigned)opt_capture;
        snprintf(ref.name, sizeof(ref.name), "%s", "cli");
    } else if (scan_pcm((unsigned)card, 0, &ref) < 0) {
        log_line("TINYALSA_CAPTURE_PCM_ABSENT card=%d", card);
        return -ENODEV;
    }
    int rc = pcm_open_dev(&c->in, ref.card, ref.device, rate, channels, 1);
    if (rc) {
        log_line("TINYALSA_CAPTURE_OPEN_FAILED card=%u device=%u rc=%d", ref.card,
                 ref.device, rc);
        return rc;
    }
    /* 上行路由是 "SLIMBUS_0_TX" 这类双值位掩码控件，不在后端里猜位定义 */
    log_line("TINYALSA_CAPTURE_OPEN card=%u device=%u dai=%s", ref.card, ref.device,
             ref.name);
    log_line("TINYALSA_CAPTURE_ROUTE_HINT=%s", ref.name);
    c->capture_frames = 0;
    return 0;
}

static void close_streams(struct dipper_client *c)
{
    if (c->out.fd >= 0) {
        log_line("TINYALSA_PCM_SUMMARY frames=%llu nonzero_samples=%llu peak_s16=%u "
                 "xruns=%d",
                 (unsigned long long)c->frames, (unsigned long long)c->nonzero_samples,
                 c->peak, c->out.xruns);
        pcm_close_dev(&c->out);
    }
    if (c->in.fd >= 0) {
        log_line("TINYALSA_CAPTURE_SUMMARY frames=%llu xruns=%d",
                 (unsigned long long)c->capture_frames, c->in.xruns);
        pcm_close_dev(&c->in);
    }
}

/* 软件音量：Q15 定点缩放，只在本后端内部生效，不改动内核控件 */
static void apply_gain(struct dipper_client *c, unsigned char *data, unsigned bytes)
{
    if (c->gain_q15 == 32768)
        return;
    for (unsigned i = 0; i + 1 < bytes; i += 2) {
        int16_t sample = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i + 1] << 8));
        int32_t scaled = ((int32_t)sample * c->gain_q15) >> 15;
        if (scaled > 32767)
            scaled = 32767;
        if (scaled < -32768)
            scaled = -32768;
        data[i] = (unsigned char)(scaled & 0xff);
        data[i + 1] = (unsigned char)((scaled >> 8) & 0xff);
    }
}

static int dispatch_capture(struct dipper_client *c,
                            const struct dipper_pal_request *h, unsigned char *data,
                            uint32_t *written, uint64_t *value)
{
    switch (h->op) {
    case DIPPER_PAL_CAPTURE_OPEN:
        return open_capture(c, h->arg0 ? h->arg0 : 48000, h->arg1 ? h->arg1 : 2);
    case DIPPER_PAL_CAPTURE_START:
        if (c->in.fd < 0)
            return -EBADF;
        return pcm_prepare(&c->in);
    case DIPPER_PAL_CAPTURE_STOP:
        if (c->in.fd < 0)
            return -EBADF;
        return ioctl(c->in.fd, DIPPER_IOCTL_DROP) < 0 ? -errno : 0;
    case DIPPER_PAL_CAPTURE_CLOSE:
        pcm_close_dev(&c->in);
        return 0;
    case DIPPER_PAL_CAPTURE_STATUS:
        return 0;
    case DIPPER_PAL_CAPTURE_READ: {
        if (c->in.fd < 0)
            return -EBADF;
        if (!h->arg0 || h->arg0 > DIPPER_PAL_CAPTURE_CHUNK || (h->arg0 & 3))
            return -EINVAL;
        int rc = pcm_transfer(&c->in, data, h->arg0, 1);
        if (rc < 0)
            return rc;
        if (mic_muted)
            memset(data, 0, (size_t)rc);
        *written = (uint32_t)rc;
        c->capture_frames += (uint64_t)rc / (c->in.channels * 2u);
        *value = c->capture_frames;
        return 0;
    }
    case DIPPER_PAL_CAPTURE_MUTE:
    case DIPPER_PAL_MIC_MUTE:
        if (h->arg0 > 1)
            return -EINVAL;
        mic_muted = (int)h->arg0;
        return 0;
    case DIPPER_PAL_CAPTURE_GET_MUTE:
    case DIPPER_PAL_MIC_GET_MUTE:
        *value = (uint64_t)mic_muted;
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}

static int dispatch(struct dipper_client *c, const struct dipper_pal_request *h,
                    unsigned char *data, uint32_t *written, uint64_t *value)
{
    if (h->op == DIPPER_PAL_OPEN)
        return open_playback(c, h->arg0, h->arg1);
    if (h->op == DIPPER_PAL_CLOSE) {
        close_streams(c);
        return 0;
    }
    if (h->op == DIPPER_PAL_STATUS)
        return 0;
    if (h->op >= DIPPER_PAL_CAPTURE_OPEN)
        return dispatch_capture(c, h, data, written, value);
    if (c->out.fd < 0)
        return -ENODEV;
    int rc;
    switch (h->op) {
    case DIPPER_PAL_START:
        if (c->out.state == DIPPER_PCM_RUNNING)
            return 0;
        /* 不主动 START：交给 start_threshold，避免空缓冲立刻欠载 */
        c->out.state = DIPPER_PCM_RUNNING;
        return 0;
    case DIPPER_PAL_STOP:
        if (c->out.state == DIPPER_PCM_OPEN)
            return 0;
        (void)ioctl(c->out.fd, DIPPER_IOCTL_DRAIN);
        rc = ioctl(c->out.fd, DIPPER_IOCTL_DROP) < 0 ? -errno : 0;
        if (!rc)
            c->out.state = DIPPER_PCM_OPEN;
        return rc;
    case DIPPER_PAL_PAUSE: {
        int enable = 1;
        if (c->out.state != DIPPER_PCM_RUNNING)
            return -EINVAL;
        rc = ioctl(c->out.fd, DIPPER_IOCTL_PAUSE, &enable) < 0 ? -errno : 0;
        if (!rc)
            c->out.state = DIPPER_PCM_PAUSED;
        return rc;
    }
    case DIPPER_PAL_RESUME: {
        int enable = 0;
        if (c->out.state != DIPPER_PCM_PAUSED)
            return -EINVAL;
        rc = ioctl(c->out.fd, DIPPER_IOCTL_PAUSE, &enable) < 0 ? -errno : 0;
        if (!rc)
            c->out.state = DIPPER_PCM_RUNNING;
        return rc;
    }
    case DIPPER_PAL_VOLUME:
        if (h->arg0 > 1000000u)
            return -EINVAL;
        c->gain_q15 = (int)((uint64_t)h->arg0 * 32768u / 1000000u);
        log_line("TINYALSA_VOLUME gain_q15=%d", c->gain_q15);
        return 0;
    case DIPPER_PAL_WRITE:
        if (c->out.state != DIPPER_PCM_RUNNING || !h->bytes || (h->bytes & 3))
            return -EINVAL;
        while (*written < h->bytes) {
            unsigned n = h->bytes - *written;
            if (n > 960)
                n = 960;
            apply_gain(c, data + *written, n);
            rc = pcm_transfer(&c->out, data + *written, n, 0);
            if (rc < 0)
                return rc;
            for (int i = 0; i < rc; i += 2) {
                int sample = (int16_t)((unsigned)data[*written + (unsigned)i] |
                                       ((unsigned)data[*written + (unsigned)i + 1] << 8));
                unsigned magnitude = (unsigned)(sample < 0 ? -sample : sample);
                if (magnitude)
                    c->nonzero_samples++;
                if (magnitude > c->peak)
                    c->peak = magnitude;
            }
            if (!c->frames)
                log_line("TINYALSA_FIRST_PCM_WRITE bytes=%d", rc);
            *written += (unsigned)rc;
            c->frames += (unsigned)rc / 4u;
        }
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}

/* ------------------------------------------------------------------ 服务端 */

static void end_signal(int sig)
{
    (void)sig;
    __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
}

static int serve(void)
{
    signal(SIGTERM, end_signal);
    signal(SIGINT, end_signal);
    signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    size_t name_len = strlen(opt_socket);
    if (name_len + 1 > sizeof(a.sun_path)) {
        close(fd);
        return -ENAMETOOLONG;
    }
    memcpy(a.sun_path + 1, opt_socket, name_len);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + name_len + 1);
    if (bind(fd, (void *)&a, len) || listen(fd, DIPPER_MAX_CLIENTS)) {
        int rc = -errno;
        close(fd);
        return rc;
    }
    struct dipper_client cs[DIPPER_MAX_CLIENTS];
    for (int i = 0; i < DIPPER_MAX_CLIENTS; i++) {
        memset(&cs[i], 0, sizeof(cs[i]));
        cs[i].fd = -1;
        cs[i].out.fd = -1;
        cs[i].in.fd = -1;
    }
    static struct {
        struct dipper_pal_request h;
        unsigned char data[DIPPER_PAL_MAX_DATA];
    } packet;
    log_line("PAL_SERVER_READY=1 pid=%d socket=@%s backend=tinyalsa", (int)getpid(),
             opt_socket);
    while (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) {
        struct pollfd fds[DIPPER_MAX_CLIENTS + 1];
        fds[0].fd = fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        for (int i = 0; i < DIPPER_MAX_CLIENTS; i++) {
            fds[i + 1].fd = cs[i].fd;
            fds[i + 1].events = POLLIN;
            fds[i + 1].revents = 0;
        }
        if (poll(fds, DIPPER_MAX_CLIENTS + 1, 200) < 0)
            continue;
        if (fds[0].revents & POLLIN) {
            int nfd = accept4(fd, 0, 0, SOCK_CLOEXEC);
            if (nfd >= 0) {
                struct ucred cred;
                socklen_t n = sizeof(cred);
                memset(&cred, 0, sizeof(cred));
                /* 与 PAL 服务保持同一准入名单：root / audio / audio_host */
                if (getsockopt(nfd, SOL_SOCKET, SO_PEERCRED, &cred, &n) ||
                    (cred.uid != 0 && cred.uid != 1041 && cred.uid != 3127)) {
                    close(nfd);
                } else {
                    struct timeval send_timeout = {1, 0};
                    if (setsockopt(nfd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                                   sizeof(send_timeout))) {
                        close(nfd);
                        continue;
                    }
                    int slot;
                    for (slot = 0; slot < DIPPER_MAX_CLIENTS && cs[slot].fd >= 0; slot++) {
                    }
                    if (slot == DIPPER_MAX_CLIENTS) {
                        close(nfd);
                    } else {
                        memset(&cs[slot], 0, sizeof(cs[slot]));
                        cs[slot].fd = nfd;
                        cs[slot].out.fd = -1;
                        cs[slot].in.fd = -1;
                    }
                }
            }
        }
        for (int i = 0; i < DIPPER_MAX_CLIENTS; i++) {
            struct dipper_client *c = &cs[i];
            if (c->fd < 0 || !fds[i + 1].revents)
                continue;
            ssize_t n = recv(c->fd, &packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
            if (n <= 0) {
                close_streams(c);
                close(c->fd);
                c->fd = -1;
                continue;
            }
            if ((size_t)n < sizeof(packet.h) || packet.h.magic != DIPPER_PAL_MAGIC ||
                packet.h.bytes > DIPPER_PAL_MAX_DATA ||
                (size_t)n != sizeof(packet.h) + packet.h.bytes ||
                (packet.h.op != DIPPER_PAL_WRITE && packet.h.bytes)) {
                close_streams(c);
                close(c->fd);
                c->fd = -1;
                continue;
            }
            struct dipper_pal_reply reply = {.magic = DIPPER_PAL_MAGIC,
                                             .sequence = packet.h.sequence};
            uint64_t value = c->frames;
            reply.rc = dispatch(c, &packet.h, packet.data, &reply.bytes, &value);
            reply.written_frames = value;
            int with_data = packet.h.op == DIPPER_PAL_CAPTURE_READ && !reply.rc &&
                            reply.bytes;
            struct iovec response[2] = {{&reply, sizeof(reply)},
                                        {packet.data, reply.bytes}};
            struct msghdr message = {.msg_iov = response, .msg_iovlen = with_data ? 2 : 1};
            size_t response_size = sizeof(reply) + (with_data ? reply.bytes : 0);
            if (sendmsg(c->fd, &message, MSG_NOSIGNAL) != (ssize_t)response_size) {
                close_streams(c);
                close(c->fd);
                c->fd = -1;
            }
            if (packet.h.op != DIPPER_PAL_WRITE || reply.rc)
                log_line("PAL_SERVER_REQUEST op=%u rc=%d bytes=%u frames=%llu",
                         packet.h.op, reply.rc, reply.bytes,
                         (unsigned long long)reply.written_frames);
        }
    }
    for (int i = 0; i < DIPPER_MAX_CLIENTS; i++) {
        if (cs[i].fd < 0)
            continue;
        close_streams(&cs[i]);
        close(cs[i].fd);
    }
    close(fd);
    return 0;
}

static void usage(void)
{
    log_line("usage: dipper-tinyalsa-server [--card N] [--playback N] [--capture N]"
             " [--socket NAME] [--route FILE] [--period FRAMES] [--periods N]"
             " [--dump-mixer]");
}

static int parse_dev(const char *text, int *out)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || value < 0 || value > 255)
        return -1;
    *out = (int)value;
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(arg, "--dump-mixer")) {
            opt_dump_mixer = 1;
        } else if (!strcmp(arg, "--help")) {
            usage();
            return 0;
        } else if (!value) {
            usage();
            return 2;
        } else if (!strcmp(arg, "--card")) {
            int card;
            if (parse_dev(value, &card) < 0)
                return 2;
            opt_card = (unsigned)card;
            i++;
        } else if (!strcmp(arg, "--playback")) {
            if (parse_dev(value, &opt_playback) < 0)
                return 2;
            i++;
        } else if (!strcmp(arg, "--capture")) {
            if (parse_dev(value, &opt_capture) < 0)
                return 2;
            i++;
        } else if (!strcmp(arg, "--socket")) {
            opt_socket = value;
            i++;
        } else if (!strcmp(arg, "--route")) {
            opt_route = value;
            i++;
        } else if (!strcmp(arg, "--period")) {
            unsigned long frames = strtoul(value, NULL, 10);
            if (!frames || frames > 48000)
                return 2;
            opt_period = (unsigned)frames;
            i++;
        } else if (!strcmp(arg, "--periods")) {
            unsigned long periods = strtoul(value, NULL, 10);
            if (!periods || periods > 64)
                return 2;
            opt_periods = (unsigned)periods;
            i++;
        } else {
            usage();
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    int card = opt_card == ~0u ? find_card("sdm845") : (int)opt_card;
    if (card < 0) {
        /* 声卡还没注册时给出明确结论，交给上层决定是否重试 */
        log_line("TINYALSA_CARD_ABSENT");
        return 3;
    }
    if (opt_dump_mixer) {
        dump_mixer((unsigned)card);
        return 0;
    }
    log_line("TINYALSA_CARD=%d socket=@%s", card, opt_socket);
    return serve() ? 4 : 0;
}

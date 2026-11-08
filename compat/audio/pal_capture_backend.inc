/* 采集构建显式开启，原播放构建保持关闭 */
#ifndef DIPPER_CAPTURE_EXPERIMENTAL
#define DIPPER_CAPTURE_EXPERIMENTAL 0
#endif
static uint64_t capture_card_epoch;
static int capture_monitor_ready;
static int32_t capture_card_event(uint32_t event, uint32_t *data, uint64_t cookie)
{
    (void)data; (void)cookie;
    if (event==PAL_SND_CARD_STATE)
        __atomic_add_fetch(&capture_card_epoch,1,__ATOMIC_RELEASE);
    return 0;
}
static int capture_card_online(void)
{
    if (!get_param || !capture_monitor_ready) return -95;
    void *payload=0; size_t size=0;
    int rc=get_param(PAL_PARAM_ID_SNDCARD_STATE,&payload,&size,0);
    if (rc) return rc;
    if (!payload || size!=sizeof(card_status_t)) return -71;
    card_status_t state;
    memcpy(&state,payload,sizeof(state));
    return state==CARD_STATUS_ONLINE?0:-19;
}
static void capture_monitor_init(void)
{
    if (!DIPPER_CAPTURE_EXPERIMENTAL || !register_global || !get_param) return;
    int rc=register_global(capture_card_event,0);
    capture_monitor_ready=!rc;
    printf("PAL_CAPTURE_CARD_MONITOR_RC=%d\n",rc);
}
static int capture_api_ready(void)
{
    return DIPPER_CAPTURE_EXPERIMENTAL && stream_read && !capture_card_online();
}

static int close_capture(struct client *c)
{
    if (!c->stream) { c->capture=0; c->state=0; return 0; }
    int rc=c->state>=2?stream_stop(c->stream):0;
    int close_rc=stream_close(c->stream);
    c->stream=0; c->state=0; c->capture=0;
    return rc?rc:close_rc;
}

static int close_client_stream(struct client *c)
{
    return c->capture?close_capture(c):close_stream(c);
}

static int open_capture(struct client *c, unsigned rate, unsigned channels)
{
    if (!capture_api_ready()) return -95;
    if (c->stream) return -16;
    if (rate!=48000 || channels!=2) return -95;
    struct { struct pal_stream_attributes value; unsigned char tail[128]; } a={0};
    struct { struct pal_device value; unsigned char tail[128]; } d={0};
    a.value.type=PAL_STREAM_LOW_LATENCY;
    a.value.direction=PAL_AUDIO_INPUT;
    a.value.in_media_config.sample_rate=rate;
    a.value.in_media_config.bit_width=16;
    a.value.in_media_config.aud_fmt_id=PAL_AUDIO_FMT_PCM_S16_LE;
    a.value.in_media_config.ch_info.channels=channels;
    a.value.in_media_config.ch_info.ch_map[0]=PAL_CHMAP_CHANNEL_FL;
    a.value.in_media_config.ch_info.ch_map[1]=PAL_CHMAP_CHANNEL_FR;
    d.value.id=PAL_DEVICE_IN_HANDSET_MIC;
    d.value.config=a.value.in_media_config;
    uint64_t epoch=__atomic_load_n(&capture_card_epoch,__ATOMIC_ACQUIRE);
    int rc=stream_open(&a.value,1,&d.value,0,0,0,0,&c->stream);
    if (rc) return rc;
    if (!c->stream) return -5;
    c->capture=1; c->state=1; c->frames=0; c->capture_epoch=epoch;
    pal_buffer_config_t input={.buf_count=2,.buf_size=DIPPER_PAL_CAPTURE_CHUNK};
    rc=stream_buffer(c->stream,&input,0);
    if (!rc && (capture_card_online() ||
        epoch!=__atomic_load_n(&capture_card_epoch,__ATOMIC_ACQUIRE))) rc=-19;
    if (rc) (void)close_capture(c);
    return rc;
}

static int dispatch_capture(struct client *c, const struct dipper_pal_request *h,
    unsigned char *data, uint32_t *bytes, uint64_t *value)
{
    *bytes=0; *value=c->frames;
    if (h->op==DIPPER_PAL_CAPTURE_STATUS)
        return DIPPER_CAPTURE_EXPERIMENTAL && stream_read?capture_card_online():-95;
    if (h->op==DIPPER_PAL_MIC_MUTE) {
        if (!mic_set_mute) return -95;
        return h->arg0<=1?mic_set_mute((bool)h->arg0):-22;
    }
    if (h->op==DIPPER_PAL_MIC_GET_MUTE) {
        if (!mic_get_mute) return -95;
        bool mute=false;
        int rc=mic_get_mute(&mute);
        if (!rc) *value=mute;
        return rc;
    }
    if (h->op==DIPPER_PAL_CAPTURE_OPEN) return open_capture(c,h->arg0,h->arg1);
    if (h->op==DIPPER_PAL_CAPTURE_CLOSE) return close_capture(c);
    if (h->op==DIPPER_PAL_CAPTURE_STOP) return close_capture(c);
    if (h->op==DIPPER_PAL_CAPTURE_START) {
        if (!c->stream) {
            int rc=open_capture(c,48000,2);
            if (rc) return rc;
        }
        if (capture_card_online() || c->capture_epoch!=
            __atomic_load_n(&capture_card_epoch,__ATOMIC_ACQUIRE)) return -19;
        if (c->state==2) return 0;
        int rc=stream_start(c->stream);
        if (!rc && (capture_card_online() || c->capture_epoch!=
            __atomic_load_n(&capture_card_epoch,__ATOMIC_ACQUIRE))) rc=-19;
        if (!rc) c->state=2;
        else (void)close_capture(c);
        return rc;
    }
    if (!c->capture || !c->stream) return -19;
    if (h->op==DIPPER_PAL_CAPTURE_MUTE) {
        if (!stream_set_mute) return -95;
        if (h->arg0>1) return -22;
        return stream_set_mute(c->stream,(bool)h->arg0);
    }
    if (h->op==DIPPER_PAL_CAPTURE_GET_MUTE) {
        if (!stream_get_mute) return -95;
        bool mute=false;
        int rc=stream_get_mute(c->stream,&mute);
        if (!rc) *value=mute;
        return rc;
    }
    if (h->op!=DIPPER_PAL_CAPTURE_READ) return -95;
    if (c->state!=2 || !h->arg0 || h->arg0>DIPPER_PAL_CAPTURE_CHUNK || (h->arg0&3)) return -22;
    if (capture_card_online() || c->capture_epoch!=
        __atomic_load_n(&capture_card_epoch,__ATOMIC_ACQUIRE)) return -19;
    struct { struct pal_buffer value; unsigned char tail[64]; } buffer={0};
    buffer.value.buffer=data; buffer.value.size=h->arg0;
    buffer.value.alloc_info.alloc_handle=-1;
    ssize_t received=stream_read(c->stream,&buffer.value);
    if (received<0) return (int)received;
    if (capture_card_online() || c->capture_epoch!=
        __atomic_load_n(&capture_card_epoch,__ATOMIC_ACQUIRE)) return -19;
    if (!received || (uint64_t)received>h->arg0 || (received&3)) return -5;
    *bytes=(unsigned)received; c->frames+=(unsigned)received/4; *value=c->frames;
    return 0;
}

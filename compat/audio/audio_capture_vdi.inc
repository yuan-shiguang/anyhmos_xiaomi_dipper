/* 仅记录采集边界和原始返回值，不输出音频内容 */
static int capture_start(struct IAudioCaptureVdi *self);
static int capture_stop(struct IAudioCaptureVdi *self);
static int capture_mute(struct IAudioCaptureVdi *self, bool mute);
static int capture_get_mute(struct IAudioCaptureVdi *self, bool *mute);
static int create_capture(struct IAudioAdapterVdi *self, const struct AudioDeviceDescriptorVdi *desc,
    const struct AudioSampleAttributesVdi *attrs, struct IAudioCaptureVdi **out);
static int destroy_capture(struct IAudioAdapterVdi *self, struct IAudioCaptureVdi *capture);
static int capture_capability(struct IAudioAdapterVdi *self, const struct AudioPortVdi *port,
    struct AudioPortCapabilityVdi *cap);

_Static_assert(offsetof(struct IAudioCaptureVdi,CaptureFrame)==0,"capture frame slot");
_Static_assert(offsetof(struct IAudioCaptureVdi,SetMute)==0x28,"capture mute slot");
_Static_assert(offsetof(struct IAudioCaptureVdi,Start)==0xc0,"capture start slot");
_Static_assert(offsetof(struct IAudioCaptureVdi,Stop)==0xc8,"capture stop slot");

struct capture {
    struct IAudioCaptureVdi api;
    pthread_mutex_t lock;
    int fd, running;
    struct AudioSampleAttributesVdi attrs;
};

static int capture_query(unsigned op, unsigned arg, struct dipper_pal_reply *reply)
{
    int fd=dipper_pal_connect();
    if (fd<0) return fd;
    int rc=dipper_pal_request(fd,op,arg,0,0,0,reply);
    close(fd);
    return rc;
}
static int capture_available(void)
{
    struct dipper_pal_reply reply={0};
    return capture_query(DIPPER_PAL_CAPTURE_STATUS,0,&reply);
}
static int capture_command(struct capture *c, unsigned op, unsigned arg,
    struct dipper_pal_reply *reply)
{
    return dipper_pal_request(c->fd,op,arg,0,0,0,reply);
}
static int capture_start_impl(struct IAudioCaptureVdi *self)
{
    struct capture *c=(void *)self; struct dipper_pal_reply reply={0};
    pthread_mutex_lock(&c->lock);
    int rc=capture_command(c,DIPPER_PAL_CAPTURE_START,0,&reply);
    if (!rc) c->running=1;
    pthread_mutex_unlock(&c->lock); return rc;
}
static int capture_stop_impl(struct IAudioCaptureVdi *self)
{
    struct capture *c=(void *)self; struct dipper_pal_reply reply={0};
    pthread_mutex_lock(&c->lock);
    int rc=capture_command(c,DIPPER_PAL_CAPTURE_STOP,0,&reply);
    if (!rc) c->running=0;
    pthread_mutex_unlock(&c->lock); return rc;
}
static int capture_frame(struct IAudioCaptureVdi *self, int8_t *frame,
    uint32_t *length, uint64_t *reply_bytes)
{
    if (!self || !frame || !length || !reply_bytes) return -EINVAL;
    unsigned capacity=*length; *length=0; *reply_bytes=0;
    if (!capacity || (capacity&3)) return -EINVAL;
    struct capture *c=(void *)self;
    unsigned copied=0;
    pthread_mutex_lock(&c->lock);
    int rc=c->running?0:-EPIPE;
    /* 按请求长度拼接采集块，保留底层真实短读 */
    while (!rc && copied<capacity) {
        unsigned chunk=capacity-copied;
        if (chunk>DIPPER_PAL_CAPTURE_CHUNK) chunk=DIPPER_PAL_CAPTURE_CHUNK;
        struct dipper_pal_reply reply={0};
        rc=dipper_pal_capture_read(c->fd,frame+copied,chunk,&reply);
        if (rc) break;
        copied+=reply.bytes;
        if (reply.bytes<chunk) break;
    }
    if (!rc) { *length=copied; *reply_bytes=copied; }
    pthread_mutex_unlock(&c->lock); return rc;
}
static int capture_mute_impl(struct IAudioCaptureVdi *self, bool mute)
{
    struct capture *c=(void *)self; struct dipper_pal_reply reply={0};
    pthread_mutex_lock(&c->lock);
    int rc=capture_command(c,DIPPER_PAL_CAPTURE_MUTE,mute,&reply);
    pthread_mutex_unlock(&c->lock); return rc;
}
static int capture_get_mute_impl(struct IAudioCaptureVdi *self, bool *mute)
{
    if (!mute) return -EINVAL;
    struct capture *c=(void *)self; struct dipper_pal_reply reply={0};
    pthread_mutex_lock(&c->lock);
    int rc=capture_command(c,DIPPER_PAL_CAPTURE_GET_MUTE,0,&reply);
    if (!rc && reply.written_frames>1) rc=-EPROTO;
    if (!rc) *mute=(bool)reply.written_frames;
    pthread_mutex_unlock(&c->lock); return rc;
}
static int adapter_mic_mute(struct IAudioAdapterVdi *self, bool mute)
{
    (void)self; struct dipper_pal_reply reply={0};
    return capture_query(DIPPER_PAL_MIC_MUTE,mute,&reply);
}
static int adapter_mic_get_mute(struct IAudioAdapterVdi *self, bool *mute)
{
    (void)self; if (!mute) return -EINVAL;
    struct dipper_pal_reply reply={0};
    int rc=capture_query(DIPPER_PAL_MIC_GET_MUTE,0,&reply);
    if (!rc && reply.written_frames>1) rc=-EPROTO;
    if (!rc) *mute=(bool)reply.written_frames;
    return rc;
}
static int capture_size(struct IAudioCaptureVdi *self, uint64_t *size)
{ (void)self; if (!size) return -EINVAL; *size=4; return 0; }
static int capture_count(struct IAudioCaptureVdi *self, uint64_t *count)
{ (void)self; if (!count) return -EINVAL; *count=240; return 0; }
static int capture_buffer_size(struct IAudioCaptureVdi *self, uint64_t *size)
{ (void)self; if (!size) return -EINVAL; *size=DIPPER_PAL_CAPTURE_CHUNK; return 0; }
static int capture_attrs(struct IAudioCaptureVdi *self, struct AudioSampleAttributesVdi *attrs)
{ if (!attrs) return -EINVAL; *attrs=((struct capture *)self)->attrs; return 0; }
static int capture_no_pause(struct IAudioCaptureVdi *self, bool *pause, bool *resume)
{ (void)self; if (!pause || !resume) return -EINVAL; *pause=false; *resume=false; return 0; }
static int create_capture_impl(struct IAudioAdapterVdi *self, const struct AudioDeviceDescriptorVdi *desc,
    const struct AudioSampleAttributesVdi *attrs, struct IAudioCaptureVdi **out)
{
    if (!desc || !attrs || !out) return -EINVAL;
    *out=0;
    if (desc->pins!=PIN_VDI_IN_MIC || attrs->sampleRate!=48000 || attrs->channelCount!=2 ||
        attrs->format!=AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT || !attrs->interleaved || attrs->isBigEndian ||
        attrs->type!=AUDIO_VDI_IN_MEDIA) return -ENOTSUP;
    int rc=capture_available(); if (rc) return rc;
    struct capture *c=calloc(1,sizeof(*c)); if (!c) return -ENOMEM;
    c->fd=dipper_pal_connect();
    if (c->fd<0) { rc=c->fd; free(c); return rc; }
    struct dipper_pal_reply reply={0};
    rc=dipper_pal_request(c->fd,DIPPER_PAL_CAPTURE_OPEN,48000,2,0,0,&reply);
    if (rc) { close(c->fd); free(c); return rc; }
    rc=pthread_mutex_init(&c->lock,0);
    if (rc) { (void)capture_command(c,DIPPER_PAL_CAPTURE_CLOSE,0,&reply); close(c->fd); free(c); return -rc; }
    c->attrs=*attrs;
#define CAPTURE_OPTIONAL(field) c->api.field=(__typeof__(c->api.field))unsupported
    CAPTURE_OPTIONAL(CaptureFrameEc); CAPTURE_OPTIONAL(GetCapturePosition);
    CAPTURE_OPTIONAL(CheckSceneCapability); CAPTURE_OPTIONAL(SelectScene);
    CAPTURE_OPTIONAL(SetVolume); CAPTURE_OPTIONAL(GetVolume); CAPTURE_OPTIONAL(GetGainThreshold);
    CAPTURE_OPTIONAL(GetGain); CAPTURE_OPTIONAL(SetGain); CAPTURE_OPTIONAL(SetSampleAttributes);
    CAPTURE_OPTIONAL(GetCurrentChannelId); CAPTURE_OPTIONAL(SetExtraParams); CAPTURE_OPTIONAL(GetExtraParams);
    CAPTURE_OPTIONAL(ReqMmapBuffer); CAPTURE_OPTIONAL(GetMmapPosition); CAPTURE_OPTIONAL(AddAudioEffect);
    CAPTURE_OPTIONAL(RemoveAudioEffect); CAPTURE_OPTIONAL(Pause); CAPTURE_OPTIONAL(Resume);
    CAPTURE_OPTIONAL(Flush); CAPTURE_OPTIONAL(AudioDevDump);
#undef CAPTURE_OPTIONAL
    c->api.CaptureFrame=capture_frame; c->api.Start=capture_start; c->api.Stop=capture_stop;
    c->api.SetMute=capture_mute; c->api.GetMute=capture_get_mute;
    c->api.GetFrameSize=capture_size; c->api.GetFrameCount=capture_count;
    c->api.GetFrameBufferSize=capture_buffer_size; c->api.GetSampleAttributes=capture_attrs;
    c->api.TurnStandbyMode=capture_stop; c->api.IsSupportsPauseAndResume=capture_no_pause;
    __atomic_add_fetch(&((struct adapter *)self)->captures,1,__ATOMIC_RELEASE);
    *out=&c->api; return 0;
}
static int destroy_capture_impl(struct IAudioAdapterVdi *self, struct IAudioCaptureVdi *capture)
{
    if (!capture) return -EINVAL;
    struct capture *c=(void *)capture; struct dipper_pal_reply reply={0};
    pthread_mutex_lock(&c->lock);
    int rc=capture_command(c,DIPPER_PAL_CAPTURE_CLOSE,0,&reply);
    close(c->fd); pthread_mutex_unlock(&c->lock); pthread_mutex_destroy(&c->lock); free(c);
    __atomic_sub_fetch(&((struct adapter *)self)->captures,1,__ATOMIC_RELEASE);
    return rc;
}
static int capture_capability_impl(struct IAudioAdapterVdi *self, const struct AudioPortVdi *port,
    struct AudioPortCapabilityVdi *cap)
{
    if (!port || port->dir!=PORT_VDI_IN) return capability(self,port,cap);
    if (!cap) return -EINVAL;
    int rc=capture_available(); if (rc) return rc;
    memset(cap,0,sizeof(*cap));
    cap->formats=calloc(1,sizeof(*cap->formats)); cap->subPorts=calloc(1,sizeof(*cap->subPorts));
    cap->supportSampleFormats=calloc(1,sizeof(*cap->supportSampleFormats));
    if (!cap->formats || !cap->subPorts || !cap->supportSampleFormats) goto fail;
    cap->subPorts[0].desc=strdup("Dipper stock303 microphone");
    if (!cap->subPorts[0].desc) goto fail;
    cap->deviceId=port->portId; cap->hardwareMode=true;
    cap->formatNum=1; cap->formatsLen=1; cap->formats[0]=AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT;
    cap->sampleRateMasks=AUDIO_VDI_SAMPLE_RATE_MASK_48000;
    cap->channelMasks=AUDIO_VDI_CHANNEL_STEREO; cap->channelCount=2;
    cap->subPortsLen=1; cap->subPorts[0].portId=port->portId;
    cap->subPorts[0].mask=PORT_VDI_PASSTHROUGH_LPCM;
    cap->supportSampleFormatsLen=1; cap->supportSampleFormats[0]=AUDIO_VDI_SAMPLE_FORMAT_S16;
    return 0;
fail:
    free(cap->formats); free(cap->subPorts); free(cap->supportSampleFormats);
    memset(cap,0,sizeof(*cap)); return -ENOMEM;
}

static int capture_start(struct IAudioCaptureVdi *self)
{
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=start phase=enter\n");
    int rc=capture_start_impl(self);
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=start phase=leave rc=%d\n",rc);
    return rc;
}
static int capture_stop(struct IAudioCaptureVdi *self)
{
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=stop phase=enter\n");
    int rc=capture_stop_impl(self);
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=stop phase=leave rc=%d\n",rc);
    return rc;
}
static int capture_mute(struct IAudioCaptureVdi *self, bool mute)
{
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=mute phase=enter\n");
    int rc=capture_mute_impl(self,mute);
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=mute phase=leave rc=%d\n",rc);
    return rc;
}
static int capture_get_mute(struct IAudioCaptureVdi *self, bool *mute)
{
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=get_mute phase=enter\n");
    int rc=capture_get_mute_impl(self,mute);
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=get_mute phase=leave rc=%d\n",rc);
    return rc;
}
static int create_capture(struct IAudioAdapterVdi *self, const struct AudioDeviceDescriptorVdi *desc,
    const struct AudioSampleAttributesVdi *attrs, struct IAudioCaptureVdi **out)
{
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=create phase=enter\n");
    if (desc && attrs) {
        fprintf(stderr,"DIPPER_MIC_ARGS pins=%u rate=%u channels=%u format=%u type=%u interleaved=%u big_endian=%u\n",
            (unsigned)desc->pins,attrs->sampleRate,attrs->channelCount,(unsigned)attrs->format,
            (unsigned)attrs->type,(unsigned)attrs->interleaved,(unsigned)attrs->isBigEndian);
    }
    int rc=create_capture_impl(self,desc,attrs,out);
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=create phase=leave rc=%d\n",rc);
    return rc;
}
static int destroy_capture(struct IAudioAdapterVdi *self, struct IAudioCaptureVdi *capture)
{
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=destroy phase=enter\n");
    int rc=destroy_capture_impl(self,capture);
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=destroy phase=leave rc=%d\n",rc);
    return rc;
}
static int capture_capability(struct IAudioAdapterVdi *self, const struct AudioPortVdi *port,
    struct AudioPortCapabilityVdi *cap)
{
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=capability phase=enter\n");
    if (port) fprintf(stderr,"DIPPER_MIC_PORT direction=%u id=%u\n",(unsigned)port->dir,port->portId);
    int rc=capture_capability_impl(self,port,cap);
    fprintf(stderr,"DIPPER_MIC_BOUNDARY op=capability phase=leave rc=%d\n",rc);
    return rc;
}

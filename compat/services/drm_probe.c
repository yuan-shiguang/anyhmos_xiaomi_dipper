/* SPDX-License-Identifier: MIT
 * Read-only KMS enumeration; explicit --draw briefly displays native color bars.
 * Uses standard Linux UAPI only. No partition or filesystem image writes.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>

#define LIMIT 256
static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped=1; }
static int call(int fd,unsigned long req,void *arg) {
    int r;do{r=ioctl(fd,req,arg);}while(r<0&&errno==EINTR&&!stopped);return r;
}
static void error(const char *s) { fprintf(stderr,"%s: errno=%d %s\n",s,errno,strerror(errno)); }
static int open_node(const char *sys,const char *node) {
    char path[512];unsigned major_n,minor_n;
    snprintf(path,sizeof(path),"%s/dev",sys);FILE *f=fopen(path,"r");if(!f)return -1;
    int n=fscanf(f,"%u:%u",&major_n,&minor_n);fclose(f);if(n!=2){errno=EINVAL;return -1;}
    if(mknod(node,S_IFCHR|0600,makedev(major_n,minor_n))<0&&errno!=EEXIST)return -1;
    struct stat st;if(lstat(node,&st)<0)return -1;
    if(!S_ISCHR(st.st_mode)||st.st_rdev!=makedev(major_n,minor_n)){errno=ENODEV;return -1;}
    return open(node,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
}
static int native(const struct drm_mode_modeinfo *m) {
    return (m->hdisplay==2248&&m->vdisplay==1080)||(m->hdisplay==1080&&m->vdisplay==2248);
}
static void print_mode(const struct drm_mode_modeinfo *m) {
    printf("mode %.32s %ux%u refresh=%u clock=%u flags=0x%x type=0x%x%s\n",
        m->name,m->hdisplay,m->vdisplay,m->vrefresh,m->clock,m->flags,m->type,native(m)?" native-dipper":"");
}
struct buffer { uint32_t handle,pitch,fb;uint64_t size;int dmafd,dumb;void *map; };
static void release(int fd,struct buffer *b) {
    if(b->fb)call(fd,DRM_IOCTL_MODE_RMFB,&b->fb);
    if(b->map&&b->map!=MAP_FAILED)munmap(b->map,b->size);
    if(b->handle){if(b->dumb){struct drm_mode_destroy_dumb d={.handle=b->handle};call(fd,DRM_IOCTL_MODE_DESTROY_DUMB,&d);}
        else {struct drm_gem_close c={.handle=b->handle};call(fd,DRM_IOCTL_GEM_CLOSE,&c);}}
    if(b->dmafd>=0)close(b->dmafd);
}
static int allocate(int fd,uint32_t w,uint32_t h,struct buffer *b) {
    struct drm_mode_create_dumb d={.height=h,.width=w,.bpp=32};
    if(call(fd,DRM_IOCTL_MODE_CREATE_DUMB,&d)==0){
        b->dumb=1;b->handle=d.handle;b->pitch=d.pitch;b->size=d.size;
        struct drm_mode_map_dumb map={.handle=d.handle};
        if(call(fd,DRM_IOCTL_MODE_MAP_DUMB,&map)<0){error("MAP_DUMB");return -1;}
        b->map=mmap(NULL,b->size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,map.offset);
        if(b->map==MAP_FAILED){error("mmap dumb");return -1;}
        printf("buffer dumb handle=%u pitch=%u bytes=%"PRIu64"\n",b->handle,b->pitch,b->size);return 0;
    }
    error("CREATE_DUMB; trying DMA heap");
    const char *heaps[]={"system","qcom,system","qcom,display"};
    b->pitch=(w*4+127)&~127U;b->size=((uint64_t)b->pitch*h+4095)&~4095ULL;
    for(unsigned i=0;i<sizeof(heaps)/sizeof(heaps[0]);i++){
        char sys[256],node[256];snprintf(sys,sizeof(sys),"/sys/class/dma_heap/%s",heaps[i]);
        snprintf(node,sizeof(node),"/dev/hmos-heap-%u",i);int heap=open_node(sys,node);if(heap<0)continue;
        struct dma_heap_allocation_data a={.len=b->size,.fd_flags=O_RDWR|O_CLOEXEC};
        int r=call(heap,DMA_HEAP_IOCTL_ALLOC,&a);close(heap);if(r<0){error("DMA_HEAP_ALLOC");continue;}
        struct drm_prime_handle p={.fd=(int)a.fd};
        if(call(fd,DRM_IOCTL_PRIME_FD_TO_HANDLE,&p)<0){error("PRIME_FD_TO_HANDLE");close(a.fd);continue;}
        b->dmafd=a.fd;b->handle=p.handle;b->map=mmap(NULL,b->size,PROT_READ|PROT_WRITE,MAP_SHARED,b->dmafd,0);
        if(b->map==MAP_FAILED){error("mmap dma buffer");return -1;}
        printf("buffer heap=%s handle=%u pitch=%u bytes=%"PRIu64"\n",heaps[i],b->handle,b->pitch,b->size);return 0;
    }
    return -1;
}
static int bars(int fd,uint32_t connector,uint32_t crtc,const struct drm_mode_modeinfo *mode,unsigned hold) {
    int result=1,committed=0;struct buffer b={.dmafd=-1};
    struct drm_mode_crtc old={.crtc_id=crtc};if(call(fd,DRM_IOCTL_MODE_GETCRTC,&old)<0){error("GETCRTC snapshot");return 1;}
    printf("KMS SNAPSHOT crtc=%u mode_valid=%u fb=%u %ux%u\n",crtc,old.mode_valid,old.fb_id,old.mode.hdisplay,old.mode.vdisplay);
    /* Firmware splash may report a mode without any restorable DRM FB. */
    if(old.mode_valid&&old.fb_id){struct drm_mode_fb_cmd fbcheck={.fb_id=old.fb_id};if(call(fd,DRM_IOCTL_MODE_GETFB,&fbcheck)<0){printf("KMS old framebuffer is not a registered DRM object; restore disabled state\n");old.mode_valid=0;}
        else if(fbcheck.handle){struct drm_gem_close c={.handle=fbcheck.handle};call(fd,DRM_IOCTL_GEM_CLOSE,&c);}}
    else old.mode_valid=0;
    if(call(fd,DRM_IOCTL_SET_MASTER,NULL)<0&&errno!=EBUSY){error("SET_MASTER");return 1;}
    if(allocate(fd,mode->hdisplay,mode->vdisplay,&b)<0)goto out;
    if(b.pitch<(uint32_t)mode->hdisplay*4||b.size<(uint64_t)b.pitch*mode->vdisplay||b.size>128*1024*1024ULL){fprintf(stderr,"Invalid buffer bounds\n");goto out;}
    struct dma_buf_sync sync={.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_WRITE};
    if(b.dmafd>=0&&call(b.dmafd,DMA_BUF_IOCTL_SYNC,&sync)<0){error("DMA sync start");goto out;}
    const uint32_t colors[]={0x00ffffff,0x00ffff00,0x0000ffff,0x0000ff00,0x00ff00ff,0x00ff0000,0x000000ff,0x00000000};
    memset(b.map,0,b.size);
    for(unsigned y=0;y<mode->vdisplay;y++){uint32_t *row=(uint32_t*)((char*)b.map+(size_t)y*b.pitch);
        for(unsigned x=0;x<mode->hdisplay;x++)row[x]=colors[x*8/mode->hdisplay];}
    if(b.dmafd>=0){sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE;if(call(b.dmafd,DMA_BUF_IOCTL_SYNC,&sync)<0){error("DMA sync end");goto out;}}
    struct drm_mode_fb_cmd2 fb={.width=mode->hdisplay,.height=mode->vdisplay,.pixel_format=DRM_FORMAT_XRGB8888};
    fb.handles[0]=b.handle;fb.pitches[0]=b.pitch;
    if(call(fd,DRM_IOCTL_MODE_ADDFB2,&fb)<0){error("ADDFB2 XRGB8888");goto out;}b.fb=fb.fb_id;
    struct drm_mode_crtc set={.set_connectors_ptr=(uintptr_t)&connector,.count_connectors=1,.crtc_id=crtc,.fb_id=b.fb,.mode_valid=1,.mode=*mode};
    if(call(fd,DRM_IOCTL_MODE_SETCRTC,&set)<0){error("SETCRTC color bars");goto out;}committed=1;
    struct drm_mode_crtc got={.crtc_id=crtc};if(call(fd,DRM_IOCTL_MODE_GETCRTC,&got)<0){error("GETCRTC verify");goto out;}
    printf("KMS COMMIT crtc=%u connector=%u fb=%u actual_fb=%u active=%u %ux%u hold=%us\n",crtc,connector,b.fb,got.fb_id,got.mode_valid,got.mode.hdisplay,got.mode.vdisplay,hold);
    if(got.fb_id!=b.fb||!got.mode_valid||got.mode.hdisplay!=mode->hdisplay||got.mode.vdisplay!=mode->vdisplay){fprintf(stderr,"KMS readback mismatch\n");goto out;}
    struct drm_crtc_get_sequence before={.crtc_id=crtc},after={.crtc_id=crtc};
    int have_sequence=call(fd,DRM_IOCTL_CRTC_GET_SEQUENCE,&before)==0;
    result=0;unsigned waits=0;struct timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);
    do {
        union drm_wait_vblank wait={.request={.type=_DRM_VBLANK_RELATIVE,.sequence=1}};
        /* This diagnostic selects the first compatible CRTC; find its UAPI index. */
        uint32_t ids[LIMIT];struct drm_mode_card_res r={.crtc_id_ptr=(uintptr_t)ids,.count_crtcs=LIMIT};
        if(call(fd,DRM_IOCTL_MODE_GETRESOURCES,&r)<0||r.count_crtcs>LIMIT){error("CRTC index");result=1;break;}
        unsigned index=0;while(index<r.count_crtcs&&ids[index]!=crtc)index++;
        if(index>=r.count_crtcs){fprintf(stderr,"CRTC absent\n");result=1;break;}
        wait.request.type|=index<<_DRM_VBLANK_HIGH_CRTC_SHIFT;
        if(call(fd,DRM_IOCTL_WAIT_VBLANK,&wait)<0){error("WAIT_VBLANK");result=1;break;}waits++;
        clock_gettime(CLOCK_MONOTONIC,&now);
    }while(!stopped&&((double)(now.tv_sec-start.tv_sec)+(now.tv_nsec-start.tv_nsec)/1e9)<hold);
    printf("KMS VBLANK_WAIT completed=%u\n",waits);
    if(have_sequence&&call(fd,DRM_IOCTL_CRTC_GET_SEQUENCE,&after)==0){
        uint64_t frames=after.sequence-before.sequence;int64_t ns=after.sequence_ns-before.sequence_ns;
        printf("KMS SCANOUT active=%u frames=%"PRIu64" time_ns=%"PRId64" measured_hz=%.3f\n",after.active,frames,ns,ns>0?(double)frames*1e9/(double)ns:0.0);
        if(!after.active||!frames){fprintf(stderr,"No scanout progress\n");result=1;}
    }else printf("KMS sequence query unsupported; no frame-count claim\n");
out:
    if(committed){old.set_connectors_ptr=old.mode_valid?(uintptr_t)&connector:0;old.count_connectors=old.mode_valid?1:0;
        if(!old.mode_valid){old.fb_id=0;old.x=0;old.y=0;memset(&old.mode,0,sizeof(old.mode));}
        if(call(fd,DRM_IOCTL_MODE_SETCRTC,&old)<0){error("Restore previous CRTC");result=1;}else printf("KMS RESTORE previous_mode_valid=%u previous_fb=%u\n",old.mode_valid,old.fb_id);}
    release(fd,&b);call(fd,DRM_IOCTL_DROP_MASTER,NULL);return result;
}
int main(int argc,char **argv) {
    int draw=0;unsigned hold=8;setvbuf(stdout,NULL,_IONBF,0);signal(SIGINT,stop);signal(SIGTERM,stop);
    for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--draw"))draw=1;else if(!strcmp(argv[i],"--hold")&&i+1<argc){hold=(unsigned)strtoul(argv[++i],NULL,10);if(hold<1||hold>300)return 2;}else{fprintf(stderr,"Usage: drm-probe [--draw] [--hold 1..300]\n");return 2;}}
    int fd=open_node("/sys/class/drm/card0","/dev/hmos-drm");if(fd<0){error("open DRM card0");return 1;}
    char name[128]={0},date[128]={0},desc[256]={0};struct drm_version v={.name_len=sizeof(name)-1,.name=name,.date_len=sizeof(date)-1,.date=date,.desc_len=sizeof(desc)-1,.desc=desc};
    if(call(fd,DRM_IOCTL_VERSION,&v)==0)printf("DRM %.127s %d.%d.%d %.127s %.255s\n",name,v.version_major,v.version_minor,v.version_patchlevel,date,desc);
    struct drm_get_cap cap={.capability=DRM_CAP_DUMB_BUFFER};if(call(fd,DRM_IOCTL_GET_CAP,&cap)==0)printf("DUMB_BUFFER=%"PRIu64"\n",(uint64_t)cap.value);
    uint32_t connectors[LIMIT],crtcs[LIMIT],encoders[LIMIT],fbs[LIMIT];
    struct drm_mode_card_res res={0};if(call(fd,DRM_IOCTL_MODE_GETRESOURCES,&res)<0){error("GETRESOURCES sizes");close(fd);return 1;}
    if(res.count_connectors>LIMIT||res.count_crtcs>LIMIT||res.count_encoders>LIMIT||res.count_fbs>LIMIT){fprintf(stderr,"Resource limit\n");close(fd);return 1;}
    res.connector_id_ptr=(uintptr_t)connectors;res.crtc_id_ptr=(uintptr_t)crtcs;res.encoder_id_ptr=(uintptr_t)encoders;res.fb_id_ptr=(uintptr_t)fbs;
    if(call(fd,DRM_IOCTL_MODE_GETRESOURCES,&res)<0||res.count_connectors>LIMIT||res.count_crtcs>LIMIT){error("GETRESOURCES values");close(fd);return 1;}
    printf("RESOURCES connectors=%u crtcs=%u encoders=%u fbs=%u bounds=%ux%u..%ux%u\n",res.count_connectors,res.count_crtcs,res.count_encoders,res.count_fbs,res.min_width,res.min_height,res.max_width,res.max_height);
    uint32_t selected=0,selected_crtc=0;struct drm_mode_modeinfo selected_mode={0};
    for(unsigned i=0;i<res.count_connectors;i++){
        struct drm_mode_get_connector c={.connector_id=connectors[i]};if(call(fd,DRM_IOCTL_MODE_GETCONNECTOR,&c)<0){error("GETCONNECTOR sizes");continue;}
        if(c.count_modes>LIMIT||c.count_props>LIMIT||c.count_encoders>LIMIT)continue;
        struct drm_mode_modeinfo modes[LIMIT];uint32_t props[LIMIT],ens[LIMIT];uint64_t values[LIMIT];
        c.modes_ptr=(uintptr_t)modes;c.props_ptr=(uintptr_t)props;c.prop_values_ptr=(uintptr_t)values;c.encoders_ptr=(uintptr_t)ens;
        if(call(fd,DRM_IOCTL_MODE_GETCONNECTOR,&c)<0||c.count_modes>LIMIT||c.count_encoders>LIMIT){error("GETCONNECTOR values");continue;}
        printf("CONNECTOR id=%u type=%u type_id=%u connected=%u encoder=%u modes=%u mm=%ux%u\n",c.connector_id,c.connector_type,c.connector_type_id,c.connection,c.encoder_id,c.count_modes,c.mm_width,c.mm_height);
        for(unsigned j=0;j<c.count_modes;j++)print_mode(&modes[j]);
        /* drm_connector_status: connected=1 (kernel enum, stable UAPI value). */
        if(c.connection!=1||c.connector_type!=DRM_MODE_CONNECTOR_DSI)continue;
        uint32_t chosen_crtc=0;for(unsigned j=0;j<c.count_encoders;j++){struct drm_mode_get_encoder e={.encoder_id=ens[j]};if(call(fd,DRM_IOCTL_MODE_GETENCODER,&e)<0)continue;
            if(e.crtc_id){chosen_crtc=e.crtc_id;break;}for(unsigned k=0;k<res.count_crtcs&&k<32;k++)if(e.possible_crtcs&(1U<<k)){chosen_crtc=crtcs[k];break;}if(chosen_crtc)break;}
        for(unsigned j=0;j<c.count_modes;j++)if(native(&modes[j])&&chosen_crtc&&(!selected||(modes[j].type&DRM_MODE_TYPE_PREFERRED))){selected=c.connector_id;selected_crtc=chosen_crtc;selected_mode=modes[j];}
    }
    int result=0;if(draw){if(!selected){fprintf(stderr,"REFUSE DRAW: connected DSI native 2248x1080/1080x2248 mode required\n");result=1;}else result=bars(fd,selected,selected_crtc,&selected_mode,hold);}
    close(fd);return result;
}

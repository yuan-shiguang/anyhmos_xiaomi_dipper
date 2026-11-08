/* Bounded read-only dipper diagnostics: never become DRM master or grab input. */
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
static uint64_t ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
int main(int argc,char **argv) {
 unsigned seconds=argc==2?(unsigned)atoi(argv[1]):8;
 if(seconds<1||seconds>20)return 2;
 alarm(seconds+5);setbuf(stdout,NULL);
 int fd=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);if(fd<0){perror("card0");return 3;}
 struct drm_set_client_cap cap={.capability=DRM_CLIENT_CAP_UNIVERSAL_PLANES,.value=1};
 if(ioctl(fd,DRM_IOCTL_SET_CLIENT_CAP,&cap)){perror("universal planes");return 4;}
 uint32_t crtcs[32];struct drm_mode_card_res res={.crtc_id_ptr=(uintptr_t)crtcs,.count_crtcs=32};
 if(ioctl(fd,DRM_IOCTL_MODE_GETRESOURCES,&res)||res.count_crtcs>32)return 5;
 uint32_t id=0;for(unsigned i=0;i<res.count_crtcs;i++){
  struct drm_mode_crtc c={.crtc_id=crtcs[i]};
  if(!ioctl(fd,DRM_IOCTL_MODE_GETCRTC,&c)&&c.mode_valid&&(c.mode.hdisplay==1080&&c.mode.vdisplay==2248)||(c.mode.hdisplay==2248&&c.mode.vdisplay==1080)){
   id=c.crtc_id;printf("ACTIVE_MODE crtc=%u name=%s hz=%u clock=%u size=%ux%u\n",id,c.mode.name,c.mode.vrefresh,c.mode.clock,c.mode.hdisplay,c.mode.vdisplay);break;
  }
 }
 if(!id)return 6;
 struct drm_crtc_get_sequence start={.crtc_id=id},end={.crtc_id=id};
 if(ioctl(fd,DRM_IOCTL_CRTC_GET_SEQUENCE,&start)){perror("get sequence");return 7;}
 uint64_t begin=ns(),last=begin,maxGap=0,activeSum=0;unsigned changes=0,activeGaps=0;uint32_t previous=0;
 while(ns()-begin<(uint64_t)seconds*1000000000){
  struct drm_mode_get_plane p={.plane_id=90};
  if(ioctl(fd,DRM_IOCTL_MODE_GETPLANE,&p)){perror("get plane");return 8;}
  uint64_t now=ns();
  if(p.fb_id!=previous){if(previous){uint64_t gap=now-last;if(gap>maxGap)maxGap=gap;changes++;if(gap<100000000){activeGaps++;activeSum+=gap;}}previous=p.fb_id;last=now;}
  usleep(1000);
 }
 if(ioctl(fd,DRM_IOCTL_CRTC_GET_SEQUENCE,&end)||end.sequence_ns<=start.sequence_ns||end.sequence<start.sequence)return 9;
 double elapsed=(ns()-begin)/1e9;
 printf("READ_ONLY_SCANOUT seconds=%.3f sequence_timestamp_ratio=%.3f new_framebuffers=%u window_average_present=%.3f max_changing_fb_gap_ms=%.3f active=%u\n",elapsed,
  (double)(end.sequence-start.sequence)*1e9/(end.sequence_ns-start.sequence_ns),changes,changes/elapsed,maxGap/1e6,end.active);
 printf("ACTIVE_GAPS samples=%u mean_ms=%.3f idle_over100ms_excluded=1\n",activeGaps,activeGaps?(double)activeSum/activeGaps/1e6:0);
 printf("NOTE sequence_timestamps_cross_idle_not_valid_refresh_rate=1 framebuffer_sampling_not_app_fps=1 no_modeset=1 no_master=1\n");close(fd);return 0;
}

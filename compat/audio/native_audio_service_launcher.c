/* SPDX-License-Identifier: MIT - preserve init's original audio identity/token. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main(int argc,char **argv)
{
    int host=argc==2 && !strcmp(argv[1],"host");
    if (argc>1 && !host) return 1;
    const char *log=host?"/data/local/tmp/dipper-audio-host.txt":"/data/local/tmp/dipper-audio-server.txt";
    int fd=open(log,O_WRONLY|O_APPEND|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0 || dup2(fd,1)<0 || dup2(fd,2)<0) return 2;
    if (fd>2) close(fd);
    setbuf(stdout,0); setbuf(stderr,0);
    printf("AUDIO_INIT_IDENTITY uid=%d gid=%d host=%d\n",getuid(),getgid(),host);
    if (setenv("LD_PRELOAD","/compat/lib64/libdipper_audio_vdi_loader.so",1)) return 3;
    if (host) execl("/compat/bin/native_audio_hdi_publisher","native_audio_hdi_publisher","--serve",(char *)0);
    else execl("/system/bin/sa_main","sa_main","/system/profile/audio_server.json",(char *)0);
    perror("original audio exec"); return 4;
}

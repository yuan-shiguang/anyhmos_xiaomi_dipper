#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
int main(void) {
 int fd=open("/data/local/tmp/dipper-graphics/bootanimation-adapter.log",O_WRONLY|O_APPEND);
 if(fd>=0){dup2(fd,1);dup2(fd,2);if(fd>2)close(fd);}
 fprintf(stderr,"DIPPER_LAUNCH uid=%d euid=%d pid=%d\n",getuid(),geteuid(),getpid());
 if(setenv("LD_PRELOAD","/compat/lib64/libdipper_boot_vulkan.so",1)){perror("setenv");return 1;}
 execl("/compat/bin/bootanimation-real","bootanimation",(char*)0);
 perror("exec bootanimation-real");return 2;
}

/* SPDX-License-Identifier: MIT
 * Keep the donor storage daemon inside the RAM-data trial: it may not create
 * any new block-device nodes discovered from global sysfs/uevents.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void)
{
    struct sock_filter filter[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,__NR_mknodat,0,3),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[2])),
        BPF_STMT(BPF_ALU|BPF_AND|BPF_K,S_IFMT),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,S_IFBLK,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM)
    };
    struct sock_fprog program={sizeof(filter)/sizeof(filter[0]),filter};
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)||prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program)){
        perror("RAM storage isolation");return 126;
    }
    if(setenv("LD_PRELOAD","/compat/lib64/libdipper_ram_no_tee.so",1)){perror("RAM storage preload");return 126;}
    execl("/system/bin/storage_daemon","storage_daemon",(char*)0);perror("storage_daemon");return 127;
}

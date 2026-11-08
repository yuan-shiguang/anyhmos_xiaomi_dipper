/* Call the donor BluetoothHost public methods through their exported ABI.
 * No service replacement and no synthetic adapter state.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef DIPPER_CURRENT_BT_API
extern int DipperBtEnableCurrent(void *, void *);
#endif
int main(int argc, char **argv)
{
    setbuf(stdout, NULL); alarm(20);
    char action[16] = "state";
    if (argc==2 && !strcmp(argv[1],"--service")) {
        FILE *f=fopen("/data/local/tmp/dipper-bluetooth-check.args","re");
        if (!f || fscanf(f,"%15s",action)!=1) return 6;
        fclose(f);
        if (strcmp(action,"state") && strcmp(action,"enable") && strcmp(action,"scan") && strcmp(action,"current")) return 6;
        int fd=open("/data/local/tmp/dipper-bluetooth-check.txt",O_WRONLY|O_APPEND|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0 || dup2(fd,1)<0 || dup2(fd,2)<0) return 7;
        if(fd>2) close(fd);
        printf("BT_NATIVE_DIAGNOSTIC_ACTION=%s uid=%d\n",action,getuid());
    } else if (argc==2) {
        if (strlen(argv[1])>=sizeof(action)) return 6;
        strcpy(action,argv[1]);
    }
    void *lib = dlopen("/system/lib64/platformsdk/libbtframework.z.so", RTLD_NOW|RTLD_LOCAL);
    if (!lib) { printf("BT_HOST_LOAD_ERROR=%s\n", dlerror()); return 2; }
    void *(*get)(void) = dlsym(lib,"_ZN4OHOS9Bluetooth13BluetoothHost14GetDefaultHostEv");
    int (*state)(const void *, int *) = dlsym(lib,"_ZNK4OHOS9Bluetooth13BluetoothHost10GetBtStateERi");
    int (*enable)(void *) = dlsym(lib,"_ZN4OHOS9Bluetooth13BluetoothHost8EnableBtEv");
    int (*scan)(void *) = dlsym(lib,"_ZN4OHOS9Bluetooth13BluetoothHost16StartBtDiscoveryEv");
    if (!get || !state || !enable || !scan) return 3;
    void *host = get(); if (!host) return 4;
    int s = -1, rc = state(host,&s);
    printf("BT_HOST_STATE_RC=%d STATE=%d uid=%d\n",rc,s,getuid());
    if (!strcmp(action,"enable") || !strcmp(action,"current")) {
#ifdef DIPPER_CURRENT_BT_API
        if (!strcmp(action,"current")) {
            rc=DipperBtEnableCurrent(lib,host); printf("BT_HOST_CURRENT_ENABLE_RC=%d\n",rc);
        } else
#endif
        {
        rc=enable(host); printf("BT_HOST_ENABLE_RC=%d\n",rc);
        }
        for(int i=0;i<12;i++) { sleep(1); s=-1; int sr=state(host,&s);
            printf("BT_HOST_POLL=%d RC=%d STATE=%d\n",i,sr,s); }
    } else if (!strcmp(action,"scan")) {
        rc=scan(host); printf("BT_HOST_SCAN_RC=%d\n",rc);
    }
    return rc ? 5 : 0;
}

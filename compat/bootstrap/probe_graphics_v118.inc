/* SPDX-License-Identifier: MIT - optional, explicitly staged dipper HDF trial. */
static int copy_ram_asset(const char *source,const char *target,mode_t mode) {
    if(strncmp(target,"/hmos-root/",11)){errno=EINVAL;return -1;}
    if(!strncmp(target,"/hmos-root/vendor/",18)||!strncmp(target,"/hmos-root/vendor_dlkm/",23)){errno=EROFS;return -1;}
    char parent[512];if(strlen(target)>=sizeof(parent)){errno=ENAMETOOLONG;return -1;}
    strcpy(parent,target);char *slash=strrchr(parent,'/');*slash=0;if(mkdirs(parent)<0)return -1;
    int in=open(source,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(in<0)return -1;
    int out=open(target,O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,mode);
    if(out<0){close(in);return -1;}int result=fchmod(out,mode);char data[8192];
    while(result==0){ssize_t n=read(in,data,sizeof(data));if(n<0){if(errno==EINTR)continue;result=-1;break;}if(n==0)break;
        ssize_t offset=0;while(offset<n){ssize_t z=write(out,data+offset,(size_t)(n-offset));if(z<0&&errno==EINTR)continue;if(z<=0){result=-1;break;}offset+=z;}}
    int saved=errno;close(in);close(out);errno=saved;return result;
}
static int graphics_run_script(const char *script) {
    int output[2];if(pipe(output)<0)return -1;
    pid_t child=fork();if(child<0){close(output[0]);close(output[1]);return -1;}
    if(child==0){close(output[0]);dup2(output[1],1);dup2(output[1],2);close(output[1]);
        execl("/busybox","busybox","sh",script,(char*)0);_exit(127);}
    close(output[1]);char text[1024];ssize_t n;
    while((n=read(output[0],text,sizeof(text)))>0)emit(text,(size_t)n);
    close(output[0]);int status;while(waitpid(child,&status,0)<0){if(errno!=EINTR)return -1;}
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0){note("Native handle patch rejected status=%d",status);return -1;}
    return 0;
}
static int graphics_ram_root(void) {
    if(access("/hmos-hdf-graphics.hcb",F_OK)<0)return 0;
    if(mkdirs("/hmos-root/compat/lib64")<0||mkdirs("/hmos-root/compat/bin")<0)return -1;
    /* The legacy lookup alias is RAM-only; no chip_prod partition is needed. */
    const char *configs[]={"/hmos-root/system/etc/hdfconfig/hdf_default.hcb","/hmos-root/chip_prod/etc/hdfconfig/hdf_default.hcb"};
    for(unsigned i=0;i<2;i++)if(copy_ram_asset("/hmos-hdf-graphics.hcb",configs[i],0644)<0)return -1;
    if(copy_ram_asset("/graphics/libdisplay_composer_vdi_impl.z.so","/hmos-root/compat/lib64/libdisplay_composer_vdi_impl.z.so",0644)<0||
       copy_ram_asset("/graphics/libdisplay_buffer_vdi_impl.z.so","/hmos-root/compat/lib64/libdisplay_buffer_vdi_impl.z.so",0644)<0)return -1;
    const char *tests[]={"libdisplay_composer_vdi_impl.z.so","libdisplay_buffer_vdi_impl.z.so","dipper_display_scanout","dipper_kms_diagnostic","dipper_hdi_query","dipper_hdi_smq","dipper_hdi_scanout","libvulkan_freedreno.so.gz"};
    for(unsigned i=0;i<sizeof(tests)/sizeof(tests[0]);i++){char source[256],target[256];snprintf(source,sizeof(source),"/graphics/%s",tests[i]);snprintf(target,sizeof(target),"/hmos-root/data/local/tmp/dipper-graphics/%s",tests[i]);if(copy_ram_asset(source,target,0755)<0)return -1;}
    if(copy_ram_asset("/graphics/dipper-turnip.json","/hmos-root/compat/etc/vulkan/icd.d/dipper-turnip.json",0644)<0||
       copy_ram_asset("/graphics/dipper-turnip.json","/hmos-root/data/local/tmp/dipper-turnip.json",0644)<0)return -1;
    if(copy_ram_asset("/graphics/foundation-screen-only.json","/hmos-root/system/profile/foundation.json",0644)<0)return -1;
    if(copy_ram_asset("/graphics/locationhub-state.json","/hmos-root/system/profile/locationhub.json",0644)<0)return -1;
    const char *boot_assets[]={"bootanimation-real","bootanimation-launcher","libdipper_boot_vulkan.so"};
    for(unsigned i=0;i<sizeof(boot_assets)/sizeof(boot_assets[0]);i++){
        char source[256],target[256];snprintf(source,sizeof(source),"/graphics/%s",boot_assets[i]);
        snprintf(target,sizeof(target),"/hmos-root/compat/%s/%s",strstr(boot_assets[i],".so")?"lib64":"bin",boot_assets[i]);
        if(copy_ram_asset(source,target,0755)<0)return -1;
    }
    const char *boot_log="/hmos-root/data/local/tmp/dipper-graphics/bootanimation-adapter.log";
    if(textfile(boot_log,"")<0||chown(boot_log,1003,1003)<0||chmod(boot_log,0600)<0)return -1;
    const char *loader_configs[]={"ld-musl-aarch64.path","ld-musl-namespace-aarch64.ini","ld-musl-namespace-aarch64-New.ini","ld-musl-namespace-aarch64-flex.ini","ld-musl-namespace-aarch64-test.ini"};
    for(unsigned i=0;i<sizeof(loader_configs)/sizeof(loader_configs[0]);i++){
        char source[256],target[256];snprintf(source,sizeof(source),"/graphics/%s",loader_configs[i]);
        snprintf(target,sizeof(target),"/hmos-root/system/etc/%s",loader_configs[i]);
        if(copy_ram_asset(source,target,0644)<0)return -1;
    }
    if(graphics_run_script("/graphics/apply-compat-paths.sh")<0||graphics_run_script("/graphics/apply-rosen-handle.sh")<0)return -1;
    if(graphics_run_script("/graphics/apply-render-usage.sh")<0)return -1;
    if(graphics_run_script("/graphics/apply-framework-dev.sh")<0)return -1;
    if(graphics_run_script("/graphics/apply-account-hmac.sh")<0)return -1;
    if(copy_ram_asset("/graphics/libdipper_el5_unavailable.so","/hmos-root/compat/lib64/libdipper_el5_unavailable.so",0644)<0)return -1;
    if(copy_ram_asset("/graphics/libwifi_sdk.z.so","/hmos-root/compat/lib64/libwifi_sdk.z.so",0644)<0)return -1;
    if(mount("/hmos-root/compat/lib64/libwifi_sdk.z.so","/hmos-root/system/lib64/platformsdk/libwifi_sdk.z.so",NULL,MS_BIND,NULL)<0)return -1;
    note("SYSTEM_WIFI_SDK_COMPAT_BIND: real WiFi proxy with nonblocking missing-SA lookup; vendor untouched");
    if(copy_ram_asset("/graphics/libaudio_policy_client.z.so","/hmos-root/compat/lib64/libaudio_policy_client.z.so",0644)<0)return -1;
    if(mount("/hmos-root/compat/lib64/libaudio_policy_client.z.so","/hmos-root/system/lib64/platformsdk/libaudio_policy_client.z.so",NULL,MS_BIND,NULL)<0)return -1;
    note("SYSTEM_AUDIO_POLICY_COMPAT_BIND: original nonblocking query for SA3009; vendor untouched");
    if(copy_ram_asset("/graphics/libnet_tether_manager_if.z.so","/hmos-root/compat/lib64/libnet_tether_manager_if.z.so",0644)<0)return -1;
    if(mount("/hmos-root/compat/lib64/libnet_tether_manager_if.z.so","/hmos-root/system/lib64/platformsdk/libnet_tether_manager_if.z.so",NULL,MS_BIND,NULL)<0)return -1;
    if(copy_ram_asset("/graphics/libsensor_client.z.so","/hmos-root/compat/lib64/libsensor_client.z.so",0644)<0)return -1;
    if(mount("/hmos-root/compat/lib64/libsensor_client.z.so","/hmos-root/system/lib64/platformsdk/libsensor_client.z.so",NULL,MS_BIND,NULL)<0)return -1;
    note("SYSTEM_OPTIONAL_SDK_COMPAT_BIND: nonblocking hotspot SA1154 and sensor SA3601; vendor untouched");
    if(copy_ram_asset("/graphics/libsps_privacy_indicator_service_intf.z.so","/hmos-root/compat/lib64/libsps_privacy_indicator_service_intf.z.so",0644)<0)return -1;
    if(mount("/hmos-root/compat/lib64/libsps_privacy_indicator_service_intf.z.so","/hmos-root/system/lib64/libsps_privacy_indicator_service_intf.z.so",NULL,MS_BIND,NULL)<0)return -1;
    if(copy_ram_asset("/graphics/native_sa_dump","/hmos-root/data/local/tmp/native_sa_dump",0755)<0)return -1;
    if(copy_ram_asset("/graphics/native_image_alloc_probe","/hmos-root/data/local/tmp/native_image_alloc_probe",0755)<0)return -1;
    if(graphics_run_script("/graphics/apply-app-sandbox.sh")<0)return -1;
    if(copy_ram_asset("/graphics/native_display_wake","/hmos-root/compat/bin/native_display_wake",0755)<0)return -1;
    if(graphics_run_script("/graphics/apply-useriam.sh")<0||copy_ram_asset("/graphics/useriam-core.json","/hmos-root/compat/etc/useriam-core.json",0644)<0)return -1;
    note("SYSTEM_PRIVACY_SDK_COMPAT_BIND: original nonblocking SA66090 query; vendor untouched");
    if(copy_ram_asset("/graphics/libdipper_ram_no_tee.so","/hmos-root/compat/lib64/libdipper_ram_no_tee.so",0644)<0)return -1;
    if(copy_ram_asset("/graphics/init-desktop-services.cfg","/hmos-root/compat/etc/init-desktop-services.cfg",0644)<0||copy_ram_asset("/graphics/storage-ram-launcher","/hmos-root/compat/bin/storage-ram-launcher",0755)<0)return -1;
    if(copy_ram_asset("/graphics/installs-debug-start.json","/hmos-root/system/profile/installs.json",0644)<0||
       copy_ram_asset("/graphics/foundation-app-core.json","/hmos-root/data/local/tmp/foundation-app-core.json",0644)<0||
       copy_ram_asset("/graphics/capture_scanout","/hmos-root/data/local/tmp/dipper-graphics/capture_scanout",0755)<0)return -1;
    DIR *firmware=opendir("/stock-gpu-firmware");if(!firmware)return -1;struct dirent *entry;int result=0;
    while((entry=readdir(firmware))){if(entry->d_name[0]=='.')continue;char source[512],target[512];snprintf(source,sizeof(source),"/stock-gpu-firmware/%s",entry->d_name);snprintf(target,sizeof(target),"/hmos-root/compat/firmware/%s",entry->d_name);if(copy_ram_asset(source,target,0644)<0){result=-1;break;}}closedir(firmware);if(result<0)return -1;
    if(put("/sys/module/firmware_class/parameters/path","/compat/firmware")<0)return -1;
    if(copy_ram_asset("/graphics/native_input_dispatch","/hmos-root/compat/bin/native_input_dispatch",0755)<0||
       copy_ram_asset("/graphics/native_render_launcher","/hmos-root/compat/bin/native_render_launcher",0755)<0||
       copy_ram_asset("/hmos-root/system/bin/uinput","/hmos-root/compat/bin/uinput-original",0755)<0)return -1;
    if(copy_ram_asset("/graphics/libdipper_surface_diagnostic.so","/hmos-root/compat/lib64/libdipper_surface_diagnostic.so",0644)<0)return -1;
    if(copy_ram_asset("/graphics/wifi-manager-eager.json","/hmos-root/system/profile/wifi_manager_service.json",0644)<0)return -1;
    if(copy_ram_asset("/graphics/pasteboard-service-eager.json","/hmos-root/system/profile/pasteboard_service.json",0644)<0||
       copy_ram_asset("/graphics/msdp-input-only.json","/hmos-root/system/profile/msdp.json",0644)<0)return -1;
    const char *framework_profiles[]={"i18n_service.json","service_router.json","download_server.json","deviceinfoservice.json","device_manager.json"};
    for(unsigned i=0;i<sizeof(framework_profiles)/sizeof(framework_profiles[0]);i++){
        char source[256],target[256];snprintf(source,sizeof(source),"/graphics/%s",framework_profiles[i]);
        snprintf(target,sizeof(target),"/hmos-root/system/profile/%s",framework_profiles[i]);
        if(copy_ram_asset(source,target,0644)<0)return -1;
    }
    const char *wifi_assets[]={"init-platform-v116.cfg","init-wifi-v116.cfg","stage-wifi-ram.sh","start-wifi-v116.sh"};
    for(unsigned i=0;i<sizeof(wifi_assets)/sizeof(wifi_assets[0]);i++){
        char source[256],target[256];snprintf(source,sizeof(source),"/graphics/%s",wifi_assets[i]);
        snprintf(target,sizeof(target),"/hmos-root/compat/%s/%s",strstr(wifi_assets[i],".cfg")?"etc":"bin",wifi_assets[i]);
        if(copy_ram_asset(source,target,0755)<0)return -1;
    }
    const char *render_logs[]={"/hmos-root/data/local/tmp/dipper-render-stderr.txt","/hmos-root/data/local/tmp/dipper-mesa.txt"};
    for(unsigned i=0;i<2;i++)if(textfile(render_logs[i],"")<0||chown(render_logs[i],1003,1003)<0||chmod(render_logs[i],0600)<0)return -1;
    const char *input_log="/hmos-root/data/local/tmp/dipper-input-last.log";
    if(textfile(input_log,"")<0||chown(input_log,2000,2000)<0||chmod(input_log,0600)<0)return -1;
    if(graphics_run_script("/graphics/stage-device-firmware.sh")<0)return -1;
    if(graphics_run_script("/graphics/apply-touch.sh")<0)return -1;
    if(graphics_run_script("/graphics/apply-runtime-v116.sh")<0)return -1;
    if(graphics_run_script("/graphics/apply-msdp-local-v1.sh")<0)return -1;
    if(graphics_run_script("/graphics/apply-preload.sh")<0)return -1;
    struct {const char *sys,*target;gid_t group;int required;} nodes[]={
        {"/sys/class/drm/card0","/hmos-root/dev/dri/card0",1003,1},
        {"/sys/class/drm/renderD128","/hmos-root/dev/dri/renderD128",1003,0},
        {"/sys/class/dma_heap/system","/hmos-root/dev/dma_heap/system",3070,1},
        {"/sys/class/kgsl/kgsl-3d0","/hmos-root/dev/kgsl-3d0",1003,0}};
    for(unsigned i=0;i<sizeof(nodes)/sizeof(nodes[0]);i++){
        if(access(nodes[i].sys,F_OK)<0&&!nodes[i].required)continue;
        char parent[256];snprintf(parent,sizeof(parent),"%s",nodes[i].target);*strrchr(parent,'/')=0;
        if(mkdirs(parent)<0||node_from_sys(nodes[i].sys,nodes[i].target,S_IFCHR)<0||chown(nodes[i].target,0,nodes[i].group)<0||chmod(nodes[i].target,0660)<0)return -1;
    }
    if(node_from_sys("/sys/class/misc/ashmem","/hmos-root/dev/ashmem",S_IFCHR)<0||
       chown("/hmos-root/dev/ashmem",0,0)<0||chmod("/hmos-root/dev/ashmem",0666)<0)return -1;
    if(mkdirs("/hmos-root/dev/input")<0||chmod("/hmos-root/dev/input",0755)<0)return -1;
    DIR *inputs=opendir("/sys/class/input");
    if(inputs){struct dirent *input;
        while((input=readdir(inputs))){
            if(strncmp(input->d_name,"event",5)||!input->d_name[5])continue;
            const char *digit=input->d_name+5;while(*digit>='0'&&*digit<='9')digit++;
            if(*digit)continue;
            char sys[512],target[512];
            snprintf(sys,sizeof(sys),"/sys/class/input/%s",input->d_name);
            snprintf(target,sizeof(target),"/hmos-root/dev/input/%s",input->d_name);
            if(node_from_sys(sys,target,S_IFCHR)<0||chown(target,6696,6696)<0||chmod(target,0660)<0){closedir(inputs);return -1;}
            note("HMOS_INPUT_NODE: %s from real sysfs; uid/gid input",target);
        }
        closedir(inputs);
    }
    if(graphics_run_script("/graphics/apply-audio-v116.sh")<0)return -1;
    note("HMOS two-host graphics trial staged in RAM; vendor mounted read-only; private hardware libraries in /compat");return 0;
}

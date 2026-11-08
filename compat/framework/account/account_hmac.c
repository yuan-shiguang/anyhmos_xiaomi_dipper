/* SPDX-License-Identifier: MIT
 * Development-only account-file HMAC bridge. No TEE/attestation emulation.
 * Hks ABI: official OpenHarmony security_huks headers. Crypto: donor OpenSSL.
 * Only os_account_info_encryption_key + HMAC-SHA256 is handled locally.
 */
#define _GNU_SOURCE
#include "hks_api.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct hmac_ctx_st HMAC_CTX;
typedef struct evp_md_st EVP_MD;
extern HMAC_CTX *HMAC_CTX_new(void);
extern void HMAC_CTX_free(HMAC_CTX *);
extern int HMAC_Init_ex(HMAC_CTX *, const void *, int, const EVP_MD *, void *);
extern int HMAC_Update(HMAC_CTX *, const unsigned char *, size_t);
extern int HMAC_Final(HMAC_CTX *, unsigned char *, unsigned int *);
extern const EVP_MD *EVP_sha256(void);

_Static_assert(sizeof(struct HksBlob)==16,"donor aarch64 blob ABI");
_Static_assert(sizeof(struct HksParam)==24,"donor aarch64 param ABI");
static const char alias[]="os_account_info_encryption_key";
static const char keypath[]="/data/service/el1/public/account/dipper-dev-hmac.key";
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static void wipe(void *p,size_t n){volatile unsigned char *v=p;while(n--)*v++=0;}
static uint64_t counter=1;
struct Session { uint64_t id; HMAC_CTX *ctx; };
static struct Session sessions[32];
#define MAGIC UINT64_C(0x53484d4100000000)
#define FAIL HKS_ERROR_BAD_STATE
#define RELAY(name, args) do { __typeof__(&name) f=(__typeof__(&name))dlsym(RTLD_NEXT,#name); return f ? f args : HKS_ERROR_NOT_SUPPORTED; } while(0)

static int ours(const struct HksBlob *a)
{return a&&a->data&&a->size==sizeof(alias)-1&&!memcmp(a->data,alias,sizeof(alias)-1);}
static int params_ok(const struct HksParamSet *p)
{
    if(!p||p->paramsCnt!=5||p->paramSetSize<8+5*sizeof(struct HksParam))return 0;
    unsigned mask=0;
    for(uint32_t i=0;i<p->paramsCnt;i++){
        uint32_t t=p->params[i].tag,v=p->params[i].uint32Param; unsigned bit;
        if(t==HKS_TAG_ALGORITHM&&v==HKS_ALG_HMAC)bit=1;
        else if(t==HKS_TAG_PURPOSE&&v==HKS_KEY_PURPOSE_MAC)bit=2;
        else if(t==HKS_TAG_KEY_SIZE&&v==256)bit=4;
        else if(t==HKS_TAG_DIGEST&&v==HKS_DIGEST_SHA256)bit=8;
        else if(t==HKS_TAG_AUTH_STORAGE_LEVEL&&v==HKS_AUTH_STORAGE_LEVEL_DE)bit=16;
        else return 0;
        if(mask&bit)return 0;mask|=bit;
    }return mask==31;
}
static int readkey(unsigned char out[32])
{
    int fd=open(keypath,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return errno==ENOENT?HKS_ERROR_NOT_EXIST:FAIL;
    struct stat s;int good=fstat(fd,&s)==0&&S_ISREG(s.st_mode)&&s.st_nlink==1&&s.st_size==32&&
        (s.st_mode&0777)==0600&&(s.st_uid==3058||s.st_uid==0);
    ssize_t n=good?read(fd,out,32):-1;close(fd);
    return n==32?0:FAIL;
}
static int generate(void)
{
    unsigned char key[32];int ret=readkey(key);
    if(ret==0){wipe(key,sizeof(key));return 0;}
    if(ret!=HKS_ERROR_NOT_EXIST)return ret;
    size_t n=0;while(n<sizeof(key)){ssize_t k=getrandom(key+n,sizeof(key)-n,0);if(k<0&&errno==EINTR)continue;if(k<=0)return FAIL;n+=(size_t)k;}
    char temporary[160];snprintf(temporary,sizeof(temporary),"%s.%d.tmp",keypath,getpid());
    int fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0){wipe(key,sizeof(key));return FAIL;}
    int good=write(fd,key,32)==32&&fsync(fd)==0;
    close(fd);wipe(key,sizeof(key));
    if(!good||rename(temporary,keypath)<0){unlink(temporary);return FAIL;}
    fprintf(stderr,"DIPPER_DEV_ACCOUNT_HMAC: software key created in RAM data; no TEE\n");return 0;
}
static uint64_t idof(const struct HksBlob *h)
{uint64_t n=0;if(h&&h->data&&h->size==8)memcpy(&n,h->data,8);return n;}
static int islocal(uint64_t n){return (n&UINT64_C(0xffffffff00000000))==MAGIC;}
static struct Session *find(uint64_t n)
{for(unsigned i=0;i<32;i++)if(sessions[i].id==n&&sessions[i].ctx)return &sessions[i];return NULL;}
int32_t HksKeyExist(const struct HksBlob *a,const struct HksParamSet *p)
{
    if(!ours(a))RELAY(HksKeyExist,(a,p));if(!params_ok(p))return HKS_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);unsigned char key[32];int r=readkey(key);wipe(key,32);pthread_mutex_unlock(&lock);return r;
}
int32_t HksGenerateKey(const struct HksBlob *a,const struct HksParamSet *p,struct HksParamSet *out)
{
    if(!ours(a))RELAY(HksGenerateKey,(a,p,out));if(!params_ok(p)||out)return HKS_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);int r=generate();pthread_mutex_unlock(&lock);return r;
}
int32_t HksInit(const struct HksBlob *a,const struct HksParamSet *p,struct HksBlob *h,struct HksBlob *token)
{
    if(!ours(a))RELAY(HksInit,(a,p,h,token));
    if(!params_ok(p)||!h||!h->data||h->size<8||token)return HKS_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);unsigned char key[32];int r=readkey(key);
    if(r==0){r=FAIL;for(unsigned i=0;i<32;i++)if(!sessions[i].ctx){
        HMAC_CTX *c=HMAC_CTX_new();if(c&&HMAC_Init_ex(c,key,32,EVP_sha256(),NULL)==1){
            uint64_t id=MAGIC|(counter++&UINT64_C(0xffffffff));sessions[i]=(struct Session){id,c};memcpy(h->data,&id,8);h->size=8;r=0;
        }else if(c)HMAC_CTX_free(c);break;
    }}wipe(key,32);pthread_mutex_unlock(&lock);return r;
}
int32_t HksUpdate(const struct HksBlob *h,const struct HksParamSet *p,const struct HksBlob *in,struct HksBlob *out)
{
    uint64_t id=idof(h);if(!islocal(id))RELAY(HksUpdate,(h,p,in,out));
    if(!params_ok(p)||!in||(!in->data&&in->size))return HKS_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);struct Session *s=find(id);int r=s&&HMAC_Update(s->ctx,in->data,in->size)==1?0:FAIL;
    if(r==0&&out)out->size=0;pthread_mutex_unlock(&lock);return r;
}
int32_t HksFinish(const struct HksBlob *h,const struct HksParamSet *p,const struct HksBlob *in,struct HksBlob *out)
{
    uint64_t id=idof(h);if(!islocal(id))RELAY(HksFinish,(h,p,in,out));
    if(!params_ok(p)||!out||!out->data||out->size<32||(in&&in->size&&!in->data))return HKS_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);struct Session *s=find(id);int r=FAIL;
    if(s){unsigned n=32;if((!in||!in->size||HMAC_Update(s->ctx,in->data,in->size)==1)&&HMAC_Final(s->ctx,out->data,&n)==1&&n==32){out->size=n;r=0;}
        HMAC_CTX_free(s->ctx);*s=(struct Session){0};}
    pthread_mutex_unlock(&lock);return r;
}
int32_t HksAbort(const struct HksBlob *h,const struct HksParamSet *p)
{
    uint64_t id=idof(h);if(!islocal(id))RELAY(HksAbort,(h,p));
    pthread_mutex_lock(&lock);struct Session *s=find(id);int r=s?0:FAIL;
    if(s){HMAC_CTX_free(s->ctx);*s=(struct Session){0};}pthread_mutex_unlock(&lock);return r;
}
#ifdef CONTRACT_MAIN
#include "hks_param.h"
static void check(int ok,const char *what){if(!ok){fprintf(stderr,"FAIL %s\n",what);exit(1);}}
int main(void)
{
    /* RFC4231 case 1 validates donor crypto without using the development key. */
    unsigned char k[20],d[32];memset(k,0x0b,20);unsigned n=32;HMAC_CTX *c=HMAC_CTX_new();
    check(c&&HMAC_Init_ex(c,k,20,EVP_sha256(),NULL)==1&&HMAC_Update(c,(const unsigned char*)"Hi There",8)==1&&HMAC_Final(c,d,&n)==1,"RFC calculation");HMAC_CTX_free(c);
    const unsigned char expected[]={0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7};
    check(n==32&&!memcmp(d,expected,32),"RFC4231");
    struct HksParam a[]={{.tag=HKS_TAG_ALGORITHM,.uint32Param=HKS_ALG_HMAC},{.tag=HKS_TAG_PURPOSE,.uint32Param=HKS_KEY_PURPOSE_MAC},{.tag=HKS_TAG_KEY_SIZE,.uint32Param=256},{.tag=HKS_TAG_DIGEST,.uint32Param=HKS_DIGEST_SHA256},{.tag=HKS_TAG_AUTH_STORAGE_LEVEL,.uint32Param=HKS_AUTH_STORAGE_LEVEL_DE}};
    struct HksParamSet *p=NULL;check(HksInitParamSet(&p)==0&&HksAddParams(p,a,5)==0&&HksBuildParamSet(&p)==0,"original param ABI");
    struct HksBlob aliasblob={sizeof(alias)-1,(uint8_t*)alias};
    check(HksGenerateKey(&aliasblob,p,NULL)==0&&HksKeyExist(&aliasblob,p)==0,"key storage");
    uint64_t id;struct HksBlob handle={8,(uint8_t*)&id},in={3,(uint8_t*)"abc"},out={32,d};
    check(HksInit(&aliasblob,p,&handle,NULL)==0,"begin");check(HksUpdate(&handle,p,&in,NULL)==0,"stream");
    struct HksBlob empty={0,NULL};check(HksFinish(&handle,p,&empty,&out)==0&&out.size==32,"finish");
    unsigned char key[32],expected2[32];check(readkey(key)==0,"key read");c=HMAC_CTX_new();n=32;
    check(c&&HMAC_Init_ex(c,key,32,EVP_sha256(),NULL)==1&&HMAC_Update(c,(const unsigned char*)"abc",3)==1&&HMAC_Final(c,expected2,&n)==1&&!memcmp(d,expected2,32),"stream output");
    HMAC_CTX_free(c);wipe(key,32);check(HksFinish(&handle,p,&empty,&out)!=0,"consumed handle rejection");
    HksFreeParamSet(&p);puts("ACCOUNT_HMAC_RFC4231_ORIGINAL_PARAM_ABI_STREAM_CONTRACT_OK");return 0;
}
#endif

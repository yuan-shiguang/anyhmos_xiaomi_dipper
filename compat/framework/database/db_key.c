/* SPDX-License-Identifier: MIT
 * Development database-key wrapping for this port, using the donor OpenSSL.
 * Only three original database AES-256-GCM aliases are accepted. Imported
 * Sks* names keep this separate from the system HUKS service and account HMAC.
 * Software keys are private to the calling UID. No TEE/authentication claims.
 */
#define _GNU_SOURCE
#include "hks_api.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
typedef struct evp_cipher_st EVP_CIPHER;
extern EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);
extern void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *);
extern const EVP_CIPHER *EVP_aes_256_gcm(void);
extern int EVP_CipherInit_ex(EVP_CIPHER_CTX *, const EVP_CIPHER *, void *, const unsigned char *, const unsigned char *, int);
extern int EVP_CipherUpdate(EVP_CIPHER_CTX *, unsigned char *, int *, const unsigned char *, int);
extern int EVP_CipherFinal_ex(EVP_CIPHER_CTX *, unsigned char *, int *);
extern int EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX *, int, int, void *);
#define FAIL HKS_ERROR_BAD_STATE
#define INVALID HKS_ERROR_INVALID_ARGUMENT
#define MAX_DATA 1024
#define HANDLE_MAGIC UINT64_C(0x5344424b00000000)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t sequence = 1;
struct Session {
    uint64_t id;
    uint32_t purpose;
    unsigned char key[32], nonce[12], aad[256], tag[16], data[MAX_DATA];
    size_t aad_len, tag_len, used;
};
static struct Session sessions[16];
static void wipe(void *p, size_t n) { volatile unsigned char *v=p; while(n--) *v++=0; }
static int random_fill(void *p, size_t n)
{
    unsigned char *v=p;
    while(n) { ssize_t k=getrandom(v,n,0); if(k<0&&errno==EINTR) continue; if(k<=0) return FAIL; v+=k; n-=(size_t)k; }
    return 0;
}
static int alias_id(const struct HksBlob *a)
{
    static const char *const names[]={"DistributedDataRdb", "distributeddb_client_root_key", "distributed_db_root_key"};
    if(!a||!a->data) return -1;
    for(unsigned i=0;i<3;i++) if(a->size==strlen(names[i])&&!memcmp(a->data,names[i],a->size)) return (int)i;
    return -1;
}
static const struct HksParam *param(const struct HksParamSet *p, uint32_t tag)
{
    if(!p||p->paramsCnt>16||p->paramSetSize<8+p->paramsCnt*sizeof(struct HksParam)) return NULL;
    const struct HksParam *found=NULL;
    for(uint32_t i=0;i<p->paramsCnt;i++) if(p->params[i].tag==tag) { if(found) return NULL; found=&p->params[i]; }
    return found;
}
static int value(const struct HksParamSet *p, uint32_t t, uint32_t v)
{ const struct HksParam *q=param(p,t); return q&&q->uint32Param==v; }
static int params_ok(const struct HksParamSet *p, int generate)
{
    if(!value(p,HKS_TAG_ALGORITHM,HKS_ALG_AES)||!value(p,HKS_TAG_BLOCK_MODE,HKS_MODE_GCM)||
       !value(p,HKS_TAG_PADDING,HKS_PADDING_NONE)||!value(p,HKS_TAG_DIGEST,0)||
       !value(p,HKS_TAG_AUTH_STORAGE_LEVEL,HKS_AUTH_STORAGE_LEVEL_DE)) return 0;
    if(generate && (!value(p,HKS_TAG_KEY_SIZE,256)||
       !value(p,HKS_TAG_PURPOSE,HKS_KEY_PURPOSE_ENCRYPT|HKS_KEY_PURPOSE_DECRYPT))) return 0;
    if(!generate&&!value(p,HKS_TAG_PURPOSE,HKS_KEY_PURPOSE_ENCRYPT)&&!value(p,HKS_TAG_PURPOSE,HKS_KEY_PURPOSE_DECRYPT)) return 0;
    for(uint32_t i=0;i<p->paramsCnt;i++) {
        uint32_t t=p->params[i].tag;
        if(t==HKS_TAG_ALGORITHM||t==HKS_TAG_KEY_SIZE||t==HKS_TAG_PURPOSE||t==HKS_TAG_BLOCK_MODE||
           t==HKS_TAG_PADDING||t==HKS_TAG_DIGEST||t==HKS_TAG_AUTH_STORAGE_LEVEL) continue;
        if(generate&&t==HKS_TAG_KEY_OVERRIDE&&!p->params[i].boolParam) continue;
        if(!generate&&(t==HKS_TAG_NONCE||t==HKS_TAG_ASSOCIATED_DATA||t==HKS_TAG_AE_TAG)) continue;
        return 0;
    }
    return 1;
}
static int keydir(char *path, size_t cap)
{
#ifdef CONTRACT_MAIN
    snprintf(path,cap,"/data/local/tmp/dipper-dbkey-contract");
#else
    uid_t uid=getuid();
    if(uid>=20000000) snprintf(path,cap,"/data/storage/el1/base/.dipper-dbkeys");
    else snprintf(path,cap,"/data/service/el1/public/database/.dipper-dbkeys/%u",(unsigned)uid);
#endif
    if(mkdir(path,0700)<0&&errno!=EEXIST) return FAIL;
    struct stat s;
    return lstat(path,&s)==0&&S_ISDIR(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==0700?0:FAIL;
}
static int readkey_at(int dir, const char *name, unsigned char key[32])
{
    int fd=openat(dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return errno==ENOENT?HKS_ERROR_NOT_EXIST:FAIL;
    struct stat s;
    int good=fstat(fd,&s)==0&&S_ISREG(s.st_mode)&&s.st_uid==getuid()&&s.st_nlink==1&&
        (s.st_mode&0777)==0600&&s.st_size==32;
    ssize_t n=good?read(fd,key,32):-1; close(fd); return n==32?0:FAIL;
}
static int key(const struct HksBlob *a, unsigned char out[32], int generate)
{
    int index=alias_id(a); if(index<0) return HKS_ERROR_NOT_SUPPORTED;
    char path[192],name[24]; if(keydir(path,sizeof(path))) return FAIL;
    int dir=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); if(dir<0) return FAIL;
    snprintf(name,sizeof(name),"aes256-gcm-%d.key",index);
    int ret=readkey_at(dir,name,out);
    if(ret==HKS_ERROR_NOT_EXIST&&generate) {
        char tmp[64]; unsigned char nonce[8];
        if(random_fill(out,32)||random_fill(nonce,sizeof(nonce))) { close(dir); wipe(out,32); return FAIL; }
        unsigned long long unique=0; memcpy(&unique,nonce,8);
        snprintf(tmp,sizeof(tmp),"new-%u-%llx.tmp",(unsigned)getpid(),unique);
        int fd=openat(dir,tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        int good=fd>=0&&write(fd,out,32)==32&&fsync(fd)==0; if(fd>=0) close(fd);
        if(good) { int k=linkat(dir,tmp,dir,name,0); good=k==0||errno==EEXIST; }
        unlinkat(dir,tmp,0);
        if(good&&fsync(dir)==0) ret=readkey_at(dir,name,out); else ret=FAIL;
    }
    close(dir); if(ret) wipe(out,32); return ret;
}
int32_t SksGenerateRandom(const struct HksParamSet *p, struct HksBlob *out)
{
    if(p||!out||!out->data||!out->size||out->size>4096) return INVALID;
    return random_fill(out->data,out->size);
}
int32_t SksKeyExist(const struct HksBlob *a, const struct HksParamSet *p)
{
    if(!params_ok(p,1)) return INVALID;
    unsigned char k[32]; int ret=key(a,k,0); wipe(k,32); return ret;
}
int32_t SksGenerateKey(const struct HksBlob *a, const struct HksParamSet *p, struct HksParamSet *out)
{
    if(out||!params_ok(p,1)) return INVALID;
    unsigned char k[32]; int ret=key(a,k,1); wipe(k,32); return ret;
}
static uint64_t idof(const struct HksBlob *h)
{ uint64_t n=0; if(h&&h->size==8&&h->data) memcpy(&n,h->data,8); return n; }
static struct Session *find(uint64_t id)
{ if(!id) return NULL; for(unsigned i=0;i<16;i++) if(sessions[i].id==id) return &sessions[i]; return NULL; }
int32_t SksInit(const struct HksBlob *a, const struct HksParamSet *p, struct HksBlob *h, struct HksBlob *token)
{
    if(!params_ok(p,0)||!h||!h->data||h->size<8||token) return INVALID;
    const struct HksParam *nonce=param(p,HKS_TAG_NONCE), *aad=param(p,HKS_TAG_ASSOCIATED_DATA), *tag=param(p,HKS_TAG_AE_TAG);
    if(!nonce||nonce->blob.size!=12||!nonce->blob.data||!aad||aad->blob.size>256||
       (aad->blob.size&&!aad->blob.data)||(tag&&(tag->blob.size!=16||!tag->blob.data))) return INVALID;
    unsigned char k[32]; int ret=key(a,k,0); if(ret) return ret;
    pthread_mutex_lock(&lock); ret=FAIL;
    for(unsigned i=0;i<16;i++) if(!sessions[i].id) {
        struct Session *s=&sessions[i]; memset(s,0,sizeof(*s));
        s->id=HANDLE_MAGIC|sequence++; s->purpose=param(p,HKS_TAG_PURPOSE)->uint32Param;
        memcpy(s->key,k,32); memcpy(s->nonce,nonce->blob.data,12); s->aad_len=aad->blob.size;
        if(s->aad_len) memcpy(s->aad,aad->blob.data,s->aad_len);
        if(tag) { memcpy(s->tag,tag->blob.data,16); s->tag_len=16; }
        memcpy(h->data,&s->id,8); h->size=8; ret=0; break;
    }
    pthread_mutex_unlock(&lock); wipe(k,32); return ret;
}
static int append(struct Session *s, const struct HksBlob *in)
{
    if(!in||(!in->data&&in->size)||in->size>MAX_DATA-s->used) return INVALID;
    if(in->size) memcpy(s->data+s->used,in->data,in->size); s->used+=in->size; return 0;
}
int32_t SksUpdate(const struct HksBlob *h, const struct HksParamSet *p, const struct HksBlob *in, struct HksBlob *out)
{
    if(!params_ok(p,0)||!out) return INVALID;
    pthread_mutex_lock(&lock); struct Session *s=find(idof(h)); int ret=s?append(s,in):FAIL;
    if(!ret) out->size=0; pthread_mutex_unlock(&lock); return ret;
}
static int aes_gcm(struct Session *s, struct HksBlob *out)
{
    int enc=s->purpose==HKS_KEY_PURPOSE_ENCRYPT;
    size_t n=s->used; unsigned char tag[16], result[MAX_DATA+32]; int used=0,last=0,dummy=0;
    if(!enc) {
        if(s->tag_len) memcpy(tag,s->tag,16);
        else { if(n<16) return INVALID; n-=16; memcpy(tag,s->data+n,16); }
    }
    size_t needed=n+(enc?16:0);
    if(!out||!out->data||out->size<needed) return HKS_ERROR_BUFFER_TOO_SMALL;
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();
    int good=ctx&&EVP_CipherInit_ex(ctx,EVP_aes_256_gcm(),NULL,NULL,NULL,enc)==1&&
        EVP_CIPHER_CTX_ctrl(ctx,9,12,NULL)==1&&EVP_CipherInit_ex(ctx,NULL,NULL,s->key,s->nonce,enc)==1&&
        EVP_CipherUpdate(ctx,NULL,&dummy,s->aad,(int)s->aad_len)==1&&
        EVP_CipherUpdate(ctx,result,&used,s->data,(int)n)==1;
    if(good&&!enc) good=EVP_CIPHER_CTX_ctrl(ctx,17,16,tag)==1;
    if(good) good=EVP_CipherFinal_ex(ctx,result+used,&last)==1;
    if(good&&enc) good=EVP_CIPHER_CTX_ctrl(ctx,16,16,result+used+last)==1;
    if(ctx) EVP_CIPHER_CTX_free(ctx);
    if(good) { out->size=(uint32_t)(used+last+(enc?16:0)); memcpy(out->data,result,out->size); }
    else out->size=0;
    wipe(result,sizeof(result)); wipe(tag,sizeof(tag)); return good?0:FAIL;
}
int32_t SksFinish(const struct HksBlob *h, const struct HksParamSet *p, const struct HksBlob *in, struct HksBlob *out)
{
    if(!params_ok(p,0)) return INVALID;
    pthread_mutex_lock(&lock); struct Session *s=find(idof(h)); int ret=FAIL;
    if(s) { ret=append(s,in); if(!ret) ret=aes_gcm(s,out); wipe(s,sizeof(*s)); }
    pthread_mutex_unlock(&lock); return ret;
}
int32_t SksAbort(const struct HksBlob *h, const struct HksParamSet *p)
{
    (void)p; pthread_mutex_lock(&lock); struct Session *s=find(idof(h)); int ret=s?0:FAIL;
    if(s) wipe(s,sizeof(*s)); pthread_mutex_unlock(&lock); return ret;
}
static int single(const struct HksBlob *a, const struct HksParamSet *p, const struct HksBlob *in, struct HksBlob *out, uint32_t purpose)
{
    if(!value(p,HKS_TAG_PURPOSE,purpose)) return INVALID;
    uint64_t h=0; struct HksBlob handle={8,(uint8_t *)&h}; int ret=SksInit(a,p,&handle,NULL);
    return ret?ret:SksFinish(&handle,p,in,out);
}
int32_t SksEncrypt(const struct HksBlob *a, const struct HksParamSet *p, const struct HksBlob *in, struct HksBlob *out)
{ return single(a,p,in,out,HKS_KEY_PURPOSE_ENCRYPT); }
int32_t SksDecrypt(const struct HksBlob *a, const struct HksParamSet *p, const struct HksBlob *in, struct HksBlob *out)
{ return single(a,p,in,out,HKS_KEY_PURPOSE_DECRYPT); }

/* Only the distributed service's original DE database alias uses software
 * wrapping. Its other aliases and CE/ECE calls keep the original HUKS route.
 * ImportKey/DeleteKey and all account/authentication imports are untouched.
 */
static int distributed_de(const struct HksBlob *a, const struct HksParamSet *p)
{ return alias_id(a)==2&&value(p,HKS_TAG_AUTH_STORAGE_LEVEL,HKS_AUTH_STORAGE_LEVEL_DE); }
int32_t DksKeyExist(const struct HksBlob *a, const struct HksParamSet *p)
{ return distributed_de(a,p)?SksKeyExist(a,p):HksKeyExist(a,p); }
int32_t DksGenerateKey(const struct HksBlob *a, const struct HksParamSet *p, struct HksParamSet *out)
{ return distributed_de(a,p)?SksGenerateKey(a,p,out):HksGenerateKey(a,p,out); }
int32_t DksEncrypt(const struct HksBlob *a, const struct HksParamSet *p, const struct HksBlob *in, struct HksBlob *out)
{ return distributed_de(a,p)?SksEncrypt(a,p,in,out):HksEncrypt(a,p,in,out); }
int32_t DksDecrypt(const struct HksBlob *a, const struct HksParamSet *p, const struct HksBlob *in, struct HksBlob *out)
{ return distributed_de(a,p)?SksDecrypt(a,p,in,out):HksDecrypt(a,p,in,out); }
int32_t DksGenerateRandom(const struct HksParamSet *p, struct HksBlob *out)
{ return p?HksGenerateRandom(p,out):SksGenerateRandom(p,out); }

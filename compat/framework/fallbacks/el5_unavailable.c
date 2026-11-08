/* SPDX-License-Identifier: MIT
 * Development port only. Report absent EL5 key service without long retries.
 * This does not generate keys, set encryption policies, or report success.
 * Preloaded only into the original installs service.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
int dipper_el5_app(uint32_t uid, const void *name, void *key)
    __asm__("_ZN4OHOS8Security11AccessToken20El5FilekeyManagerKit14GenerateAppKeyEjRKNSt3__h12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEERS9_");
int dipper_el5_group(uint32_t uid, const void *name, void *key)
    __asm__("_ZN4OHOS8Security11AccessToken20El5FilekeyManagerKit18GenerateGroupIDKeyEjRKNSt3__h12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEERS9_");
static int unavailable(uint32_t uid)
{
    static unsigned count;
    if(count++ < 4) fprintf(stderr,"DIPPER: EL5 service unavailable, uid=%u; no key or encryption policy created\n", uid);
    errno=ENOTSUP;
    return -ENOTSUP;
}
int dipper_el5_app(uint32_t uid, const void *name, void *key)
{
    (void)name;(void)key;return unavailable(uid);
}
int dipper_el5_group(uint32_t uid, const void *name, void *key)
{
    (void)name;(void)key;return unavailable(uid);
}

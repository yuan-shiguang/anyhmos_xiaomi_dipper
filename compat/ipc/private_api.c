#define ACE5_PRIVATE_IMPLEMENTATION
#include "private_api.h"
#include <linux/module.h>
#include "dipper_hmos_api_v1.h"
#include <linux/delay.h>
typeof(__list_lru_init) *ace5_api___list_lru_init;
typeof(__wake_up_pollfree) *ace5_api___wake_up_pollfree;
typeof(close_fd_get_file) *ace5_api_close_fd_get_file;
typeof(has_capability_noaudit) *ace5_api_has_capability_noaudit;
typeof(list_lru_add) *ace5_api_list_lru_add;
typeof(list_lru_count_node) *ace5_api_list_lru_count_node;
typeof(list_lru_del) *ace5_api_list_lru_del;
typeof(list_lru_destroy) *ace5_api_list_lru_destroy;
typeof(list_lru_isolate) *ace5_api_list_lru_isolate;
typeof(list_lru_walk_node) *ace5_api_list_lru_walk_node;
typeof(mmput_async) *ace5_api_mmput_async;
typeof(security_binder_set_context_mgr) *ace5_api_security_binder_set_context_mgr;
typeof(security_binder_transaction) *ace5_api_security_binder_transaction;
typeof(security_binder_transfer_binder) *ace5_api_security_binder_transfer_binder;
typeof(security_binder_transfer_file) *ace5_api_security_binder_transfer_file;
typeof(security_cred_getsecid) *ace5_api_security_cred_getsecid;
typeof(security_release_secctx) *ace5_api_security_release_secctx;
typeof(security_secid_to_secctx) *ace5_api_security_secid_to_secctx;
typeof(task_work_add) *ace5_api_task_work_add;
typeof(zap_page_range) *ace5_api_zap_page_range;
int ace5_private_api_init(void)
{
    const struct dipper_hmos_ipc_api_v1 *api = __symbol_get("dipper_hmos_ipc_api_v1");
    if (!api) {
        pr_err("DIPPER_IPC missing typed kernel API table; compatible kernel required\n");
        return -ENOENT;
    }
    if (api->magic != DIPPER_HMOS_API_MAGIC || api->version != 1 || api->size != sizeof(*api)) {
        pr_err("DIPPER_IPC kernel API table version/layout mismatch\n");
        __symbol_put("dipper_hmos_ipc_api_v1");
        return -ELIBBAD;
    }
    if (!api->api___list_lru_init) return -ELIBBAD;
    ace5_api___list_lru_init = api->api___list_lru_init;
    pr_info("DIPPER_IPC typed-canonical name=__list_lru_init address=%px\n", api->api___list_lru_init);
    if (!api->api___wake_up_pollfree) return -ELIBBAD;
    ace5_api___wake_up_pollfree = api->api___wake_up_pollfree;
    pr_info("DIPPER_IPC typed-canonical name=__wake_up_pollfree address=%px\n", api->api___wake_up_pollfree);
    if (!api->api_close_fd_get_file) return -ELIBBAD;
    ace5_api_close_fd_get_file = api->api_close_fd_get_file;
    pr_info("DIPPER_IPC typed-canonical name=close_fd_get_file address=%px\n", api->api_close_fd_get_file);
    if (!api->api_has_capability_noaudit) return -ELIBBAD;
    ace5_api_has_capability_noaudit = api->api_has_capability_noaudit;
    pr_info("DIPPER_IPC typed-canonical name=has_capability_noaudit address=%px\n", api->api_has_capability_noaudit);
    if (!api->api_list_lru_add) return -ELIBBAD;
    ace5_api_list_lru_add = api->api_list_lru_add;
    pr_info("DIPPER_IPC typed-canonical name=list_lru_add address=%px\n", api->api_list_lru_add);
    if (!api->api_list_lru_count_node) return -ELIBBAD;
    ace5_api_list_lru_count_node = api->api_list_lru_count_node;
    pr_info("DIPPER_IPC typed-canonical name=list_lru_count_node address=%px\n", api->api_list_lru_count_node);
    if (!api->api_list_lru_del) return -ELIBBAD;
    ace5_api_list_lru_del = api->api_list_lru_del;
    pr_info("DIPPER_IPC typed-canonical name=list_lru_del address=%px\n", api->api_list_lru_del);
    if (!api->api_list_lru_destroy) return -ELIBBAD;
    ace5_api_list_lru_destroy = api->api_list_lru_destroy;
    pr_info("DIPPER_IPC typed-canonical name=list_lru_destroy address=%px\n", api->api_list_lru_destroy);
    if (!api->api_list_lru_isolate) return -ELIBBAD;
    ace5_api_list_lru_isolate = api->api_list_lru_isolate;
    pr_info("DIPPER_IPC typed-canonical name=list_lru_isolate address=%px\n", api->api_list_lru_isolate);
    if (!api->api_list_lru_walk_node) return -ELIBBAD;
    ace5_api_list_lru_walk_node = api->api_list_lru_walk_node;
    pr_info("DIPPER_IPC typed-canonical name=list_lru_walk_node address=%px\n", api->api_list_lru_walk_node);
    if (!api->api_mmput_async) return -ELIBBAD;
    ace5_api_mmput_async = api->api_mmput_async;
    pr_info("DIPPER_IPC typed-canonical name=mmput_async address=%px\n", api->api_mmput_async);
    if (!api->api_security_binder_set_context_mgr) return -ELIBBAD;
    ace5_api_security_binder_set_context_mgr = api->api_security_binder_set_context_mgr;
    pr_info("DIPPER_IPC typed-canonical name=security_binder_set_context_mgr address=%px\n", api->api_security_binder_set_context_mgr);
    if (!api->api_security_binder_transaction) return -ELIBBAD;
    ace5_api_security_binder_transaction = api->api_security_binder_transaction;
    pr_info("DIPPER_IPC typed-canonical name=security_binder_transaction address=%px\n", api->api_security_binder_transaction);
    if (!api->api_security_binder_transfer_binder) return -ELIBBAD;
    ace5_api_security_binder_transfer_binder = api->api_security_binder_transfer_binder;
    pr_info("DIPPER_IPC typed-canonical name=security_binder_transfer_binder address=%px\n", api->api_security_binder_transfer_binder);
    if (!api->api_security_binder_transfer_file) return -ELIBBAD;
    ace5_api_security_binder_transfer_file = api->api_security_binder_transfer_file;
    pr_info("DIPPER_IPC typed-canonical name=security_binder_transfer_file address=%px\n", api->api_security_binder_transfer_file);
    if (!api->api_security_cred_getsecid) return -ELIBBAD;
    ace5_api_security_cred_getsecid = api->api_security_cred_getsecid;
    pr_info("DIPPER_IPC typed-canonical name=security_cred_getsecid address=%px\n", api->api_security_cred_getsecid);
    if (!api->api_security_release_secctx) return -ELIBBAD;
    ace5_api_security_release_secctx = api->api_security_release_secctx;
    pr_info("DIPPER_IPC typed-canonical name=security_release_secctx address=%px\n", api->api_security_release_secctx);
    if (!api->api_security_secid_to_secctx) return -ELIBBAD;
    ace5_api_security_secid_to_secctx = api->api_security_secid_to_secctx;
    pr_info("DIPPER_IPC typed-canonical name=security_secid_to_secctx address=%px\n", api->api_security_secid_to_secctx);
    if (!api->api_task_work_add) return -ELIBBAD;
    ace5_api_task_work_add = api->api_task_work_add;
    pr_info("DIPPER_IPC typed-canonical name=task_work_add address=%px\n", api->api_task_work_add);
    if (!api->api_zap_page_range) return -ELIBBAD;
    ace5_api_zap_page_range = api->api_zap_page_range;
    pr_info("DIPPER_IPC typed-canonical name=zap_page_range address=%px\n", api->api_zap_page_range);

    return 0;
}

/* Header inline helpers call these names before the alias macros apply. */
void __wake_up_pollfree(struct wait_queue_head *head)
{ ace5_api___wake_up_pollfree(head); }
unsigned long list_lru_count_node(struct list_lru *lru, int nid)
{ return ace5_api_list_lru_count_node(lru, nid); }
unsigned long list_lru_walk_node(struct list_lru *lru, int nid,
    list_lru_walk_cb isolate, void *arg, unsigned long *nr)
{ return ace5_api_list_lru_walk_node(lru, nid, isolate, arg, nr); }

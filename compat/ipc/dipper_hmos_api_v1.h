/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DIPPER_HMOS_API_V1_H
#define DIPPER_HMOS_API_V1_H
#include <linux/cache.h>
#include <linux/list_lru.h>
#include <linux/poll.h>
#include <linux/fdtable.h>
#include <linux/capability.h>
#include <linux/sched/mm.h>
#include <linux/security.h>
#include <linux/task_work.h>
#include <linux/mm.h>
#define DIPPER_HMOS_API_MAGIC 0x5348454e47495043ULL
struct dipper_hmos_ipc_api_v1 {
    u64 magic;
    u32 version;
    u32 size;
    typeof(__list_lru_init) *api___list_lru_init;
    typeof(__wake_up_pollfree) *api___wake_up_pollfree;
    typeof(close_fd_get_file) *api_close_fd_get_file;
    typeof(has_capability_noaudit) *api_has_capability_noaudit;
    typeof(list_lru_add) *api_list_lru_add;
    typeof(list_lru_count_node) *api_list_lru_count_node;
    typeof(list_lru_del) *api_list_lru_del;
    typeof(list_lru_destroy) *api_list_lru_destroy;
    typeof(list_lru_isolate) *api_list_lru_isolate;
    typeof(list_lru_walk_node) *api_list_lru_walk_node;
    typeof(mmput_async) *api_mmput_async;
    typeof(security_binder_set_context_mgr) *api_security_binder_set_context_mgr;
    typeof(security_binder_transaction) *api_security_binder_transaction;
    typeof(security_binder_transfer_binder) *api_security_binder_transfer_binder;
    typeof(security_binder_transfer_file) *api_security_binder_transfer_file;
    typeof(security_cred_getsecid) *api_security_cred_getsecid;
    typeof(security_release_secctx) *api_security_release_secctx;
    typeof(security_secid_to_secctx) *api_security_secid_to_secctx;
    typeof(task_work_add) *api_task_work_add;
    typeof(zap_page_range) *api_zap_page_range;
 };
#endif

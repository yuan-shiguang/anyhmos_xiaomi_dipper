// SPDX-License-Identifier: GPL-2.0
/* Read-only, typed CFI entry points for the isolated Harmony IPC module.
 * Function addresses are emitted by the C compiler as canonical jump-table
 * entries. Raw kallsyms/export implementation addresses are not used.
 * Original security/MM implementations and CFI enforcement remain intact. */
#include <linux/export.h>
#include "dipper_hmos_api_v1.h"
const struct dipper_hmos_ipc_api_v1 dipper_hmos_ipc_api_v1 = {
    .magic = DIPPER_HMOS_API_MAGIC,
    .version = 1,
    .size = sizeof(struct dipper_hmos_ipc_api_v1),
    .api___list_lru_init = __list_lru_init,
    .api___wake_up_pollfree = __wake_up_pollfree,
    .api_close_fd_get_file = close_fd_get_file,
    .api_has_capability_noaudit = has_capability_noaudit,
    .api_list_lru_add = list_lru_add,
    .api_list_lru_count_node = list_lru_count_node,
    .api_list_lru_del = list_lru_del,
    .api_list_lru_destroy = list_lru_destroy,
    .api_list_lru_isolate = list_lru_isolate,
    .api_list_lru_walk_node = list_lru_walk_node,
    .api_mmput_async = mmput_async,
    .api_security_binder_set_context_mgr = security_binder_set_context_mgr,
    .api_security_binder_transaction = security_binder_transaction,
    .api_security_binder_transfer_binder = security_binder_transfer_binder,
    .api_security_binder_transfer_file = security_binder_transfer_file,
    .api_security_cred_getsecid = security_cred_getsecid,
    .api_security_release_secctx = security_release_secctx,
    .api_security_secid_to_secctx = security_secid_to_secctx,
    .api_task_work_add = task_work_add,
    .api_zap_page_range = zap_page_range,
};
EXPORT_SYMBOL_GPL(dipper_hmos_ipc_api_v1);

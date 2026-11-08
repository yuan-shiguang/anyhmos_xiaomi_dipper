// SPDX-License-Identifier: GPL-2.0
/* OpenHarmony task token inheritance through stock Android task vendor hooks.
 * No stock task structures are modified; the table is keyed by task lifetime,
 * with explicit fork and task-free hooks. Allocation failure loses
 * the token (denies access), never inherits an unrelated object's authority.
 */
#include <linux/module.h>
#include <linux/delay.h>
static int diagnostic_stage = -1;
module_param(diagnostic_stage, int, 0400);
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/tracepoint.h>
#include <trace/hooks/sched.h>
#include "ace5_tokens.h"

struct token_pair { u64 token, first; };
struct token_entry {
    const void *key;
    struct token_pair pair;
    struct hlist_node link;
};
#define TABLE_BITS 10
static DEFINE_HASHTABLE(tasks, TABLE_BITS);
static DEFINE_SPINLOCK(table_lock);
static atomic_t allocation_failures = ATOMIC_INIT(0);

static struct token_entry *lookup(struct hlist_head *table, const void *key)
{
    struct token_entry *entry;
    hlist_for_each_entry(entry, &table[hash_min((unsigned long)key, TABLE_BITS)], link)
        if (entry->key == key)
            return entry;
    return NULL;
}

static struct token_pair get_pair(struct hlist_head *table, const void *key)
{
    struct token_pair pair = {0};
    struct token_entry *entry;
    unsigned long flags;
    spin_lock_irqsave(&table_lock, flags);
    entry = lookup(table, key);
    if (entry)
        pair = entry->pair;
    spin_unlock_irqrestore(&table_lock, flags);
    return pair;
}

static int put_pair(struct hlist_head *table, const void *key, struct token_pair pair)
{
    struct token_entry *entry, *fresh;
    unsigned long flags;
    fresh = kzalloc(sizeof(*fresh), GFP_ATOMIC);
    spin_lock_irqsave(&table_lock, flags);
    entry = lookup(table, key);
    if (entry) {
        entry->pair = pair;
    } else if (fresh) {
        fresh->key = key;
        fresh->pair = pair;
        hlist_add_head(&fresh->link, &table[hash_min((unsigned long)key, TABLE_BITS)]);
        entry = fresh;
        fresh = NULL;
    }
    spin_unlock_irqrestore(&table_lock, flags);
    kfree(fresh);
    if (!entry) {
        atomic_inc(&allocation_failures);
        return -ENOMEM;
    }
    return 0;
}

static void erase_pair(struct hlist_head *table, const void *key)
{
    struct token_entry *entry;
    unsigned long flags;
    spin_lock_irqsave(&table_lock, flags);
    entry = lookup(table, key);
    if (entry)
        hlist_del(&entry->link);
    spin_unlock_irqrestore(&table_lock, flags);
    kfree(entry);
}

u64 ace5_task_token(struct task_struct *task, bool first)
{
    struct token_pair pair = get_pair(tasks, task);
    return first ? pair.first : pair.token;
}

int ace5_set_task_token(struct task_struct *task, u64 value, bool first)
{
    struct token_pair pair = get_pair(tasks, task);
    if (first)
        pair.first = value;
    else
        pair.token = value;
    return put_pair(tasks, task, pair);
}

static void on_dup(void *data, struct task_struct *child, struct task_struct *parent)
{
    struct token_pair pair = get_pair(tasks, parent);
    pair.first = 0; /* Matches OpenHarmony fork semantics. */
    if (pair.token)
        put_pair(tasks, child, pair);
}

static void on_task_free(void *data, struct task_struct *task)
{
    erase_pair(tasks, task);
}

struct hook_entry { const char *name; void *callback; struct tracepoint *point; };
static struct hook_entry hooks[] = {
    { "android_vh_dup_task_struct", on_dup },
    { "android_vh_free_task", on_task_free },

};

static void find_hook(struct tracepoint *point, void *data)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(hooks); i++)
        if (!strcmp(point->name, hooks[i].name))
            hooks[i].point = point;
}

static void clear_table(struct hlist_head *table)
{
    unsigned int i;
    struct token_entry *entry;
    struct hlist_node *next;
    for (i = 0; i < (1 << TABLE_BITS); i++)
        hlist_for_each_entry_safe(entry, next, &table[i], link) {
            hlist_del(&entry->link);
            kfree(entry);
        }
}

static void unregister_hooks(unsigned int count)
{
    while (count--)
        tracepoint_probe_unregister(hooks[count].point, hooks[count].callback, NULL);
    tracepoint_synchronize_unregister();
}

int __init ace5_binder_init(void);
int ace5_private_api_init(void);

static int __init ace5_init(void)
{
    unsigned int i;
    int ret;
    pr_info("DIPPER_IPC init-entry diagnostic_stage=%d\n", diagnostic_stage);
    msleep(150);
    if (diagnostic_stage == 0) return -ECANCELED;
    pr_info("DIPPER_IPC private-api-begin\n");
    msleep(150);
    ret = ace5_private_api_init();
    pr_info("DIPPER_IPC private-api-end ret=%d\n", ret);
    if (ret) return ret;
    if (diagnostic_stage == 1) return -ECANCELED;
    pr_info("DIPPER_IPC enumerate-hooks-begin\n");
    msleep(150);
    for_each_kernel_tracepoint(find_hook, NULL);
    pr_info("DIPPER_IPC enumerate-hooks-end\n");
    for (i = 0; i < ARRAY_SIZE(hooks); i++)
        if (!hooks[i].point) {
            pr_err("ACE5_TOKEN: required hook missing: %s\n", hooks[i].name);
            return -ENOENT;
        }
    if (diagnostic_stage == 2) return -ECANCELED;
    for (i = 0; i < ARRAY_SIZE(hooks); i++) {
        pr_info("DIPPER_IPC register-hook index=%u name=%s\n", i, hooks[i].name);
        msleep(150);
        ret = tracepoint_probe_register(hooks[i].point, hooks[i].callback, NULL);
        if (ret) {
            unregister_hooks(i);
            goto fail;
        }
    }
    pr_info("DIPPER_IPC register-hooks-complete\n");
    msleep(150);
    if (diagnostic_stage == 3) {
        unregister_hooks(ARRAY_SIZE(hooks));
        pr_info("DIPPER_IPC diagnostic hooks removed\n");
        return -ECANCELED;
    }
    pr_info("DIPPER_IPC token-device-begin\n");
    msleep(150);
    ret = ace5_token_device_init();
    pr_info("DIPPER_IPC token-device-end ret=%d\n", ret);
    if (ret) {
        unregister_hooks(ARRAY_SIZE(hooks));
        goto fail;
    }
    pr_info("DIPPER_IPC binder-begin\n");
    msleep(150);
    ret = ace5_binder_init();
    pr_info("DIPPER_IPC binder-end ret=%d\n", ret);
    if (ret) {
        ace5_token_device_exit();
        unregister_hooks(ARRAY_SIZE(hooks));
        goto fail;
    }
    pr_info("ACE5_TOKEN: real token device and isolated Harmony Binder driver registered\n");
    return 0;
fail:
    clear_table(tasks);
    return ret;
}

/* Binder VMAs, deferred work and task hooks live until reboot. No forced unload. */
module_init(ace5_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ace5 OpenHarmony token device and isolated Binder transport");

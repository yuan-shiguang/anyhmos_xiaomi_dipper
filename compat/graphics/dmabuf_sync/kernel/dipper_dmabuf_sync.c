// SPDX-License-Identifier: GPL-2.0-only
#ifdef DIPPER_SYNC_HOST_TEST
#include "bridge_host_shim.h"
#else
#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-resv.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>
#endif
#include "../uapi/dipper_dmabuf_sync.h"

#define DIPPER_FENCE_LIMIT 4096U
#define DIPPER_CONTEXT_COUNT 0xffffffffULL

struct dipper_fences {
	struct dma_fence **items;
	unsigned int count;
	unsigned int capacity;
};

static u64 dipper_context_base;
static atomic64_t dipper_context_next = ATOMIC64_INIT(0);

// 预留唯一上下文区间，外部数组不能被误判为自建数组
static int dipper_context_initialize(void)
{
	dipper_context_base = dma_fence_context_alloc((unsigned int)DIPPER_CONTEXT_COUNT);
	if (dipper_context_base > (~0ULL - DIPPER_CONTEXT_COUNT))
		return -EOVERFLOW;
	return 0;
}

static bool dipper_owned_array(struct dma_fence *fence)
{
	return fence && dma_fence_is_array(fence) &&
		fence->context >= dipper_context_base &&
		fence->context - dipper_context_base < DIPPER_CONTEXT_COUNT;
}

static void dipper_put_fences(struct dma_fence **fences, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; ++i)
		dma_fence_put(fences[i]);
	kfree(fences);
}

static int dipper_append_fence(struct dipper_fences *output,
	struct dma_fence *fence, bool keep_error)
{
	struct dma_fence **grown;
	unsigned int i, capacity;

	if (dma_fence_is_signaled(fence) && !(keep_error && fence->error < 0))
		return 0;
	for (i = 0; i < output->count; ++i) {
		// 独立栅栏可能随后失败，只合并同一个引用对象
		if (output->items[i] == fence)
			return 0;
	}
	if (output->count == DIPPER_FENCE_LIMIT)
		return -E2BIG;
	if (output->count == output->capacity) {
		capacity = output->capacity ? output->capacity * 2 : 8;
		grown = krealloc(output->items, capacity * sizeof(*grown), GFP_KERNEL);
		if (!grown)
			return -ENOMEM;
		output->items = grown;
		output->capacity = capacity;
	}
	output->items[output->count++] = dma_fence_get(fence);
	return 0;
}

// 仅展平自建 AND，保留外部容器及其完成条件
static int dipper_collect_head(struct dipper_fences *output,
	struct dma_fence *head, bool keep_error)
{
	struct dma_fence_array *array;
	unsigned int i;
	int error;

	if (!dipper_owned_array(head))
		return dipper_append_fence(output, head, keep_error);
	if (dma_fence_is_signaled(head) && !keep_error)
		return 0;
	array = to_dma_fence_array(head);
	for (i = 0; i < array->num_fences; ++i) {
		if (dipper_owned_array(array->fences[i]))
			return -EUCLEAN;
		// 未完成集合中的已失败子项仍属于原等待结果
		error = dipper_append_fence(output, array->fences[i], true);
		if (error)
			return error;
	}
	return 0;
}

static struct dma_fence *dipper_merge_fences(struct dma_fence **heads,
	unsigned int count, struct dma_fence *extra)
{
	struct dipper_fences output = {0};
	struct dma_fence_array *array;
	struct dma_fence *result;
	u64 offset;
	unsigned int i;
	int error;

	for (i = 0; i < count; ++i) {
		error = dipper_collect_head(&output, heads[i], false);
		if (error)
			goto put_output;
	}
	if (extra) {
		error = dipper_collect_head(&output, extra, true);
		if (error)
			goto put_output;
	}
	if (!output.count) {
		kfree(output.items);
		return dma_fence_get_stub();
	}
	if (output.count == 1) {
		result = output.items[0];
		kfree(output.items);
		return result;
	}
	offset = atomic64_fetch_inc(&dipper_context_next);
	if (offset >= DIPPER_CONTEXT_COUNT) {
		error = -EOVERFLOW;
		goto put_output;
	}
	array = dma_fence_array_create(output.count, output.items,
		dipper_context_base + offset, 1, false);
	if (!array) {
		error = -ENOMEM;
		goto put_output;
	}
	return &array->base;

put_output:
	dipper_put_fences(output.items, output.count);
	return ERR_PTR(error);
}

// 所有失败点通过后才挂回调，失败请求不留下异步引用
static void dipper_publish_fence(struct dma_fence *fence)
{
	if (dipper_owned_array(fence))
		dma_fence_enable_sw_signaling(fence);
}

// 读取等待写者，写入等待当前全部访问者
static struct dma_fence *dipper_snapshot(struct dma_resv *reservation, bool write)
{
	struct dma_fence **shared = NULL;
	struct dma_fence *exclusive = NULL, *result;
	unsigned int count = 0;
	int error;

	error = dma_resv_get_fences(reservation, write ? NULL : &exclusive,
		&count, &shared);
	if (error) {
		dipper_put_fences(shared, count);
		dma_fence_put(exclusive);
		return ERR_PTR(error);
	}
	if (!write) {
		dipper_put_fences(shared, count);
		return exclusive ? exclusive : dma_fence_get_stub();
	}
	result = dipper_merge_fences(shared, count, NULL);
	dipper_put_fences(shared, count);
	return result;
}

static bool dipper_valid_transfer(const struct dipper_dmabuf_sync_file *request)
{
	return request->dma_buf_fd >= 0 && !request->reserved && request->flags &&
		!(request->flags & ~DIPPER_DMABUF_SYNC_VALID_FLAGS);
}

static long dipper_export(void __user *user)
{
	struct dipper_dmabuf_sync_file request;
	struct dma_buf *buffer;
	struct dma_fence *fence;
	struct sync_file *sync;
	int fd, error;

	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (!dipper_valid_transfer(&request) || request.fence_fd != -1)
		return -EINVAL;
	buffer = dma_buf_get(request.dma_buf_fd);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		error = fd;
		goto put_buffer;
	}
	fence = dipper_snapshot(buffer->resv, request.flags & DIPPER_DMABUF_SYNC_WRITE);
	if (IS_ERR(fence)) {
		error = PTR_ERR(fence);
		goto put_fd;
	}
	sync = sync_file_create(fence);
	dma_fence_put(fence);
	if (!sync) {
		error = -ENOMEM;
		goto put_fd;
	}
	request.fence_fd = fd;
	// 先复制描述符再安装文件，复制失败时完整回滚
	if (copy_to_user(user, &request, sizeof(request))) {
		error = -EFAULT;
		fput(sync->file);
		goto put_fd;
	}
	dipper_publish_fence(sync->fence);
	fd_install(fd, sync->file);
	dma_buf_put(buffer);
	return 0;

put_fd:
	put_unused_fd(fd);
put_buffer:
	dma_buf_put(buffer);
	return error;
}

static long dipper_import(void __user *user)
{
	struct dipper_dmabuf_sync_file request;
	struct dma_fence **previous = NULL;
	struct dma_fence *fence, *merged;
	struct dma_buf *buffer;
	unsigned int count = 0;
	int error;

	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (!dipper_valid_transfer(&request) || request.fence_fd < 0)
		return -EINVAL;
	buffer = dma_buf_get(request.dma_buf_fd);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	fence = sync_file_get_fence(request.fence_fd);
	if (!fence) {
		error = -EINVAL;
		goto put_buffer;
	}
	error = dma_resv_lock_interruptible(buffer->resv, NULL);
	if (error)
		goto put_fence;
	if (request.flags & DIPPER_DMABUF_SYNC_WRITE) {
		// 旧版独占槽会清空读者，先合并未完成依赖再替换
		error = dma_resv_get_fences(buffer->resv, NULL, &count, &previous);
		if (!error) {
			merged = dipper_merge_fences(previous, count, fence);
			if (IS_ERR(merged)) {
				error = PTR_ERR(merged);
			} else {
				dipper_publish_fence(merged);
				dma_resv_add_excl_fence(buffer->resv, merged);
				dma_fence_put(merged);
			}
		}
		dipper_put_fences(previous, count);
	} else {
		error = dma_resv_reserve_shared(buffer->resv, 1);
		if (!error)
			dma_resv_add_shared_fence(buffer->resv, fence);
	}
	dma_resv_unlock(buffer->resv);
put_fence:
	dma_fence_put(fence);
put_buffer:
	dma_buf_put(buffer);
	return error;
}

static long dipper_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct dipper_dmabuf_sync_abi query;
	void __user *user = (void __user *)argument;

	(void)file;
	switch (command) {
	case DIPPER_DMABUF_SYNC_QUERY:
		if (copy_from_user(&query, user, sizeof(query)))
			return -EFAULT;
		if (query.version || query.features || query.reserved)
			return -EINVAL;
		query.version = DIPPER_DMABUF_SYNC_VERSION;
		query.features = DIPPER_DMABUF_SYNC_FEATURES_REQUIRED;
		query.reserved = 0;
		return copy_to_user(user, &query, sizeof(query)) ? -EFAULT : 0;
	case DIPPER_DMABUF_SYNC_EXPORT:
		return dipper_export(user);
	case DIPPER_DMABUF_SYNC_IMPORT:
		return dipper_import(user);
	default:
		return -ENOTTY;
	}
}

#ifndef DIPPER_SYNC_HOST_TEST
#ifdef CONFIG_COMPAT
static long dipper_compat_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	return dipper_ioctl(file, command, (unsigned long)compat_ptr(argument));
}
#endif

static const struct file_operations dipper_operations = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = dipper_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = dipper_compat_ioctl,
#endif
	.llseek = no_llseek,
};

static struct miscdevice dipper_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "dipper_dmabuf_sync",
	.fops = &dipper_operations,
	.mode = 0600,
};

static int __init dipper_init(void)
{
	int error;

	BUILD_BUG_ON(sizeof(struct dipper_dmabuf_sync_abi) != 16);
	BUILD_BUG_ON(sizeof(struct dipper_dmabuf_sync_file) != 16);
	error = dipper_context_initialize();
	return error ? error : misc_register(&dipper_device);
}

static void __exit dipper_exit(void)
{
	misc_deregister(&dipper_device);
}

module_init(dipper_init);
module_exit(dipper_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Owned dma-buf reservation and sync-file bridge");
#endif

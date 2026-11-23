// SPDX-License-Identifier: <SPDX License Expression>

#include "mx_dma.h"

#ifndef MX_DMA_DISABLE_TRACE
#include "trace.h"
#else
#define trace_mx_dma_xfer_enqueue(dev_id, xfer_id, op, dir, sz, sg, pi, pc)	do { } while (0)
#define trace_mx_dma_xfer_wait_exit(dev_id, xfer_id, ret, state)		do { } while (0)
#endif

enum mx_dma_wait_state {
	MX_DMA_WAIT_COMPLETED		= 0,
	MX_DMA_WAIT_RECOVERED		= 1,
	MX_DMA_WAIT_LATE_COMPLETED	= 2,
	MX_DMA_WAIT_ZOMBIE		= 3,
};

static_assert(MX_DMA_WAIT_COMPLETED      == 0, "trace wait_state names out of sync");
static_assert(MX_DMA_WAIT_RECOVERED      == 1, "trace wait_state names out of sync");
static_assert(MX_DMA_WAIT_LATE_COMPLETED == 2, "trace wait_state names out of sync");
static_assert(MX_DMA_WAIT_ZOMBIE         == 3, "trace wait_state names out of sync");

static unsigned int timeout_ms = 60000;

static int timeout_ms_set(const char *val, const struct kernel_param *kp)
{
	unsigned int ms;
	int ret;

	ret = kstrtouint(val, 0, &ms);
	if (ret)
		return ret;
	if (ms == 0 || ms > INT_MAX)
		return -EINVAL;

	timeout_ms = ms;
	return 0;
}

static const struct kernel_param_ops timeout_ms_ops = {
	.set = timeout_ms_set,
	.get = param_get_uint,
};
module_param_cb(timeout_ms, &timeout_ms_ops, &timeout_ms, 0644);
__MODULE_PARM_TYPE(timeout_ms, "uint");
MODULE_PARM_DESC(timeout_ms, "Transfer and passthru command timeout in ms, 1 to 2147483647 (default 60000)");

static unsigned int parallel_count = 6;
module_param(parallel_count, uint, 0644);
static unsigned int parallel_split_ratio = 50;
module_param(parallel_split_ratio, uint, 0644);
static void mx_sg_context_release(struct mx_sg_context *ctx)
{
	struct device *dev = &ctx->mx_pdev->pdev->dev;
	struct sg_table *sgt = &ctx->sgt;

	if (sgt->nents)
		dma_unmap_sg(dev, sgt->sgl, sgt->orig_nents, ctx->dir);

	if (ctx->pages_nr > 0)
		unpin_user_pages_dirty_lock(ctx->pages, ctx->pages_nr,
					    ctx->dir != DMA_TO_DEVICE);

	if (sgt->sgl && sgt->sgl != ctx->sg_inline)
		sg_free_table(sgt);

	if (ctx->pages && ctx->pages != ctx->pages_inline)
		kvfree(ctx->pages);

	kfree(ctx);
}

static void mx_sg_context_put(struct mx_sg_context *ctx)
{
	if (!ctx)
		return;
	if (refcount_dec_and_test(&ctx->refcount))
		mx_sg_context_release(ctx);
}

static struct mx_sg_context *mx_sg_context_get(struct mx_sg_context *ctx)
{
	refcount_inc(&ctx->refcount);
	return ctx;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0) && \
	RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 2)
static inline int mx_sg_alloc_table(struct sg_table *sgt,
		struct page **pages, unsigned int n, unsigned int off,
		unsigned long sz, unsigned int max_seg, gfp_t gfp)
{
	(void)max_seg;
	return sg_alloc_table_from_pages(sgt, pages, n, off, sz, gfp);
}
#else
#define mx_sg_alloc_table sg_alloc_table_from_pages_segment
#endif

static unsigned int mx_max_sg_segment(struct device *dev)
{
	size_t limit = dma_get_max_seg_size(dev);
	size_t map_max = dma_max_mapping_size(dev);

	if (map_max)
		limit = min(limit, map_max);
	limit = ALIGN_DOWN(min_t(size_t, limit, UINT_MAX), PAGE_SIZE);
	if (!limit) {
		pr_warn_once("DMA mapping limit %zu below PAGE_SIZE; capping segments at PAGE_SIZE\n",
			     map_max);
		return PAGE_SIZE;
	}

	return (unsigned int)limit;
}

static struct mx_sg_context *mx_sg_context_create(struct mx_pci_dev *mx_pdev,
		void __user *user_addr, size_t total_size, enum dma_data_direction dir)
{
	struct mx_sg_context *ctx;
	struct sg_table *sgt;
	unsigned int offset, gup_flags = 0;
	unsigned long pages_nr;
	long pinned;
	int ret;

	offset = offset_in_page((unsigned long)user_addr);

	/* INTENTIONAL: the bound keeps kvmalloc_array(pages_nr, ptr size) <= INT_MAX. */
	if (total_size == 0 ||
	    total_size > ((size_t)INT_MAX / sizeof(struct page *)) * PAGE_SIZE - offset)
		return ERR_PTR(-EINVAL);

	pages_nr = DIV_ROUND_UP(offset + total_size, PAGE_SIZE);

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		pr_warn("Failed to alloc mx_sg_context\n");
		return ERR_PTR(-ENOMEM);
	}

	refcount_set(&ctx->refcount, 1);
	ctx->mx_pdev = mx_pdev;
	ctx->dir = dir;
	ctx->user_addr = user_addr;
	ctx->total_size = total_size;
	sgt = &ctx->sgt;

	if (pages_nr <= MX_PAGES_INLINE_NR) {
		ctx->pages = ctx->pages_inline;
	} else {
		ctx->pages = kvmalloc_array(pages_nr, sizeof(struct page *), GFP_KERNEL | __GFP_ZERO);
		if (!ctx->pages) {
			pr_warn("Failed to alloc pages\n");
			ret = -ENOMEM;
			goto err;
		}
	}

	if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL)
		gup_flags |= FOLL_WRITE;

	pinned = pin_user_pages_fast((unsigned long)user_addr, pages_nr, gup_flags, ctx->pages);
	if (pinned > 0)
		ctx->pages_nr = pinned;
	if (pinned != pages_nr) {
		const char *kind = pinned < 0 ? "failed" :
				   pinned == 0 ? "none" : "partial";

		pr_warn("pin_user_pages_fast %s (req=%lu, got=%ld)\n", kind, pages_nr, pinned);
		ret = (pinned < 0) ? (int)pinned : -EFAULT;
		goto err;
	}

	if (pages_nr <= MX_PAGES_INLINE_NR) {
		sg_init_table(ctx->sg_inline, MX_PAGES_INLINE_NR);
		sg_set_page(&ctx->sg_inline[0], ctx->pages[0], total_size, offset);
		sgt->sgl = ctx->sg_inline;
		sgt->orig_nents = pages_nr;
	} else {
		ret = mx_sg_alloc_table(sgt, ctx->pages, pages_nr, offset,
					total_size,
					mx_max_sg_segment(&mx_pdev->pdev->dev),
					GFP_KERNEL);
		if (ret) {
			pr_warn("sg_alloc_table_from_pages failed (err=%d)\n", ret);
			goto err;
		}
	}

	sgt->nents = dma_map_sg(&mx_pdev->pdev->dev, sgt->sgl, sgt->orig_nents, dir);
	if (!sgt->nents) {
		pr_warn("Failed to dma_map_sg\n");
		ret = -EIO;
		goto err;
	}

	return ctx;

err:
	mx_sg_context_put(ctx);
	return ERR_PTR(ret);
}

void desc_list_free(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer)
{
	int i;

	if (transfer->desc_list_va) {
		for (i = 0; i < transfer->desc_list_cnt; i++) {
			if (transfer->desc_list_va[i])
				dma_pool_free(mx_pdev->page_pool, transfer->desc_list_va[i], transfer->desc_list_ba[i]);
		}
		kfree(transfer->desc_list_va);
		transfer->desc_list_va = NULL;
	}

	if (transfer->desc_list_ba) {
		kfree(transfer->desc_list_ba);
		transfer->desc_list_ba = NULL;
	}
}

int desc_list_alloc(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer, int list_cnt)
{
	int i;

	transfer->desc_list_cnt = list_cnt;
	transfer->desc_list_va = kcalloc(list_cnt, sizeof(void *), GFP_KERNEL);
	if (!transfer->desc_list_va) {
		pr_warn("Failed to allocate desc_list_va\n");
		return -ENOMEM;
	}

	transfer->desc_list_ba = kcalloc(list_cnt, sizeof(dma_addr_t), GFP_KERNEL);
	if (!transfer->desc_list_ba) {
		pr_warn("Failed to allocate desc_list_ba\n");
		kfree(transfer->desc_list_va);
		transfer->desc_list_va = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < list_cnt; i++) {
		void *cpu_addr;
		dma_addr_t bus_addr;

		cpu_addr = dma_pool_alloc(mx_pdev->page_pool, GFP_ATOMIC, &bus_addr);
		if (!cpu_addr)
			goto fail;

		transfer->desc_list_va[i] = cpu_addr;
		transfer->desc_list_ba[i] = bus_addr;
	}

	return 0;

fail:
	desc_list_free(mx_pdev, transfer);
	pr_warn("Failed to dma_pool_alloc\n");

	return -ENOMEM;
}

static void free_mx_transfer(struct mx_transfer *transfer)
{
	if (transfer->command && transfer->command != (void *)transfer->cmd_inline)
		kfree(transfer->command);
	kmem_cache_free(mx_transfer_cache, transfer);
}

static void release_mx_transfer(struct mx_transfer *transfer)
{
	if (transfer->sg_ctx)
		mx_sg_context_put(transfer->sg_ctx);
	transfer_id_free(transfer);
	mx_lease_transfer_put(transfer);
	free_mx_transfer(transfer);
}

static struct mx_transfer *alloc_mx_transfer(void __user *user_addr, size_t size,
		uint64_t device_addr, enum dma_data_direction dir)
{
	struct mx_transfer *transfer;

	transfer = kmem_cache_zalloc(mx_transfer_cache, GFP_KERNEL);
	if (!transfer)
		return NULL;

	INIT_LIST_HEAD(&transfer->entry);
	INIT_LIST_HEAD(&transfer->zombie_entry);

	transfer->id = transfer_id_alloc(transfer);
	if (transfer->id < 0) {
		pr_warn("Failed to alloc transfer_id\n");
		kmem_cache_free(mx_transfer_cache, transfer);
		return NULL;
	}

	transfer->user_addr = user_addr;
	transfer->size = size;
	transfer->device_addr = device_addr;
	transfer->dir = dir;
	transfer->is_zombie = false;
	atomic_set(&transfer->wait_claimed, 0);

	return transfer;
}

static struct mx_transfer *alloc_mx_transfer_sg(struct mx_sg_context *sg_ctx,
		size_t byte_offset, size_t slice_size, uint64_t device_addr)
{
	struct mx_transfer *transfer;

	transfer = alloc_mx_transfer(NULL, slice_size, device_addr, sg_ctx->dir);
	if (!transfer)
		return NULL;

	transfer->sg_ctx = mx_sg_context_get(sg_ctx);
	transfer->sg_byte_offset = byte_offset;

	return transfer;
}

static struct mx_transfer **alloc_mx_transfers(struct mx_sg_context *sg_ctx,
		uint64_t device_addr_base, int count)
{
	struct mx_transfer **transfers;
	void __user *cursor = sg_ctx->user_addr;
	size_t remaining = sg_ctx->total_size;
	uint64_t device_addr = device_addr_base;
	int pages_nr = sg_ctx->pages_nr;
	int q, r;
	int i;

	transfers = kcalloc(count, sizeof(struct mx_transfer *), GFP_KERNEL);
	if (!transfers) {
		pr_warn("Failed to alloc parallel mx_transfer\n");
		return NULL;
	}

	q = pages_nr / count;
	r = pages_nr % count;

	for (i = 0; i < count; i++) {
		int num = (r-- > 0) ? q + 1 : q;
		uintptr_t end_addr = ((uintptr_t)cursor + (uintptr_t)num * PAGE_SIZE) & PAGE_MASK;
		size_t slice = min_t(size_t, end_addr - (uintptr_t)cursor, remaining);
		size_t byte_offset = (uintptr_t)cursor - (uintptr_t)sg_ctx->user_addr;

		transfers[i] = alloc_mx_transfer_sg(sg_ctx, byte_offset, slice, device_addr);
		if (!transfers[i]) {
			pr_warn("Failed to alloc mx_transfer[%d]\n", i);
			goto fail;
		}
		cursor = (void __user *)((uintptr_t)cursor + slice);
		remaining -= slice;
		device_addr += slice;
	}

	return transfers;

fail:
	for (i = 0; i < count; i++) {
		if (transfers[i])
			release_mx_transfer(transfers[i]);
	}
	kfree(transfers);
	return NULL;
}

static void mx_transfer_queue(struct mx_queue *queue, struct mx_transfer *transfer)
{
	init_completion(&transfer->done);
	mx_queue_submit(queue, &transfer, 1);
}

static void mx_transfer_queue_parallel(struct mx_queue *queue, struct mx_transfer **transfers, int count)
{
	int i;

	for (i = 0; i < count; i++)
		init_completion(&transfers[i]->done);
	mx_queue_submit(queue, transfers, count);
}

static bool mx_transfer_remove_from_sq(struct mx_queue *queue, struct mx_transfer *transfer)
{
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&queue->sq_lock, flags);
	if (!list_empty(&transfer->entry)) {
		list_del_init(&transfer->entry);
		found = true;
	}
	spin_unlock_irqrestore(&queue->sq_lock, flags);

	return found;
}

static void mx_transfer_destroy_sg(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer);
static int mx_transfer_destroy_ctrl(struct mx_transfer *transfer);

static ssize_t mx_transfer_teardown_done(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer)
{
	ssize_t size = transfer->size;

	if (transfer->is_sg)
		mx_transfer_destroy_sg(mx_pdev, transfer);
	else
		mx_transfer_destroy_ctrl(transfer);
	return size;
}

static void mx_transfer_teardown_recovered(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer)
{
	if (transfer->is_sg)
		mx_transfer_destroy_sg(mx_pdev, transfer);
	else
		release_mx_transfer(transfer);
}

static ssize_t mx_transfer_wait(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer)
{
	long left_time;
	ssize_t ret;
	int state __maybe_unused;
	u32 __maybe_unused xfer_id = (u32)transfer->id;
	bool lv_on = READ_ONCE(mx_pdev->liveness_enable);
	unsigned int mult = lv_on ?
		clamp(READ_ONCE(mx_pdev->liveness_max_mult), 1u, LIVENESS_MAX_MULT_CEIL) : 1;

	{
		unsigned int chunk_ms = lv_on ?
			max(min(READ_ONCE(mx_pdev->liveness_stall_ms), timeout_ms), LIVENESS_WAIT_CHUNK_MIN_MSEC) : timeout_ms;
		unsigned long hard_deadline =
			jiffies + msecs_to_jiffies(timeout_ms) * mult;

		do {
			left_time = wait_for_completion_interruptible_timeout(&transfer->done,
					msecs_to_jiffies(chunk_ms));
			if (left_time != 0)
				break;
			if (!lv_on)
				break;
			if (atomic_read(&mx_pdev->io_queue->lv_health) == MX_LIVENESS_DEAD)
				break;
		} while (time_before(jiffies, hard_deadline));
	}
	if (left_time > 0) {
		ret = mx_transfer_teardown_done(mx_pdev, transfer);
		state = MX_DMA_WAIT_COMPLETED;
		goto out;
	}

	{
		unsigned long flags;
		bool dead = lv_on &&
			    atomic_read(&mx_pdev->io_queue->lv_health) == MX_LIVENESS_DEAD;
		ssize_t fail_ret = dead ? -EIO : (left_time == 0 ? -ETIMEDOUT : -EINTR);

		if (left_time == 0) {
			if (dead)
				pr_warn("transfer failed: transport DEAD (id=%u, size=%#llx, dir=%u)\n",
						transfer->id, (uint64_t)transfer->size, transfer->dir);
			else
				pr_warn("wait_for_completion is timeout (id=%u, size=%#llx, dir=%u, ceiling=%lu ms)\n",
						transfer->id, (uint64_t)transfer->size, transfer->dir,
						(unsigned long)timeout_ms * mult);
		}
		else
			pr_warn("wait_for_completion is interrupted (id=%u, size=%#llx, dir=%u)\n",
					transfer->id, (uint64_t)transfer->size, transfer->dir);

		if (mx_transfer_remove_from_sq(mx_pdev->io_queue, transfer)) {
			mx_transfer_teardown_recovered(mx_pdev, transfer);
			ret = fail_ret;
			state = MX_DMA_WAIT_RECOVERED;
			goto out;
		}

		if (!dead) {
			if (wait_for_completion_timeout(&transfer->done,
						msecs_to_jiffies(1000)) > 0) {
				ret = mx_transfer_teardown_done(mx_pdev, transfer);
				state = MX_DMA_WAIT_LATE_COMPLETED;
				goto out;
			}
			pr_warn("mx_dma: transfer did not complete within 1s (id=%u), marking zombie\n",
					transfer->id);
		}

		WRITE_ONCE(transfer->is_zombie, true);
		transfer->zombie_timestamp = jiffies;
		atomic_inc(&mx_pdev->io_queue->zombie_wait_count);

		spin_lock_irqsave(&mx_pdev->zombie_lock, flags);
		list_add_tail(&transfer->zombie_entry, &mx_pdev->zombie_list);
		spin_unlock_irqrestore(&mx_pdev->zombie_lock, flags);

		ret = fail_ret;
		state = MX_DMA_WAIT_ZOMBIE;
		goto out;
	}
out:
	trace_mx_dma_xfer_wait_exit(mx_pdev->dev_id, xfer_id, ret, state);
	return ret;
}

static void mx_transfer_wait_work(struct work_struct *work)
{
	struct mx_transfer *transfer = container_of(work, struct mx_transfer, work);
	struct mx_pci_dev *mx_pdev = transfer->mx_pdev;

	mx_transfer_wait(mx_pdev, transfer);
	percpu_ref_put(&mx_pdev->io_ref);
}

static void mx_transfer_schedule_wait(struct mx_pci_dev *mx_pdev,
				      struct mx_transfer *transfer)
{
	if (WARN_ON_ONCE(!queue_work(mx_pdev->async_wq, &transfer->work)))
		percpu_ref_put(&mx_pdev->io_ref);
}

static int mx_transfer_init_sg(struct mx_file_ctx *ctx,
		struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer, int opcode)
{
	int ret = mx_lease_transfer_get(ctx, transfer);

	if (ret)
		return ret;
	transfer->command = mx_pdev->ops.create_command_sg(mx_pdev, transfer, opcode);
	if (IS_ERR_OR_NULL(transfer->command)) {
		int ret = transfer->command ? PTR_ERR(transfer->command) : -ENOMEM;

		transfer->command = NULL;
		pr_warn("Failed to create_command_sg (id=%u, err=%d)\n", transfer->id, ret);
		return ret;
	}

	transfer->mx_pdev = mx_pdev;
	transfer->is_sg = true;
	INIT_WORK(&transfer->work, mx_transfer_wait_work);

	return 0;
}

static void mx_transfer_destroy_sg(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer)
{
	desc_list_free(mx_pdev, transfer);
	release_mx_transfer(transfer);
}

static ssize_t mx_transfer_submit_sg_one(struct mx_file_ctx *ctx,
		struct mx_pci_dev *mx_pdev,
		struct mx_transfer *transfer, int opcode, bool nowait)
{
	size_t size = transfer->size;
	ssize_t ret;

	ret = mx_transfer_init_sg(ctx, mx_pdev, transfer, opcode);
	if (ret < 0) {
		mx_transfer_destroy_sg(mx_pdev, transfer);
		return ret;
	}

	trace_mx_dma_xfer_enqueue(mx_pdev->dev_id, transfer->id, opcode,
			transfer->dir, transfer->size, true, 0, 1);
	mx_transfer_queue(mx_pdev->io_queue, transfer);
	if (nowait) {
		percpu_ref_get(&mx_pdev->io_ref);

		mx_transfer_schedule_wait(mx_pdev, transfer);
		return size;
	}

	return mx_transfer_wait(mx_pdev, transfer);
}

static ssize_t mx_transfer_submit_sg_parallel(struct mx_file_ctx *ctx,
		struct mx_pci_dev *mx_pdev,
		struct mx_transfer **transfers, int opcode, int count, bool nowait)
{
	ssize_t transferred = 0;
	ssize_t err = 0;
	size_t total_size = 0;
	int ret = 0;
	int i;

	for (i = 0; i < count; i++) {
		ret = mx_transfer_init_sg(ctx, mx_pdev, transfers[i], opcode);
		if (ret < 0)
			break;
		total_size += transfers[i]->size;
	}

	if (ret < 0) {
		for (i = 0; i < count; i++)
			mx_transfer_destroy_sg(mx_pdev, transfers[i]);

		kfree(transfers);
		return ret;
	}

	for (i = 0; i < count; i++)
		trace_mx_dma_xfer_enqueue(mx_pdev->dev_id, transfers[i]->id, opcode,
				transfers[i]->dir, transfers[i]->size, true, i, count);

	mx_transfer_queue_parallel(mx_pdev->io_queue, transfers, count);

	if (nowait) {
		percpu_ref_get_many(&mx_pdev->io_ref, count);
		for (i = 0; i < count; i++)
			mx_transfer_schedule_wait(mx_pdev, transfers[i]);
		kfree(transfers);
		return total_size;
	}

	for (i = 0; i < count; i++) {
		ssize_t r = mx_transfer_wait(mx_pdev, transfers[i]);

		if (r < 0)
			err = r;
		else
			transferred += r;
	}

	kfree(transfers);
	return err ? err : transferred;
}

static ssize_t mx_transfer_submit_sg_split(struct mx_file_ctx *ctx,
		struct mx_pci_dev *mx_pdev,
		void __user *buf, size_t size, uint64_t device_addr,
		enum dma_data_direction dir, int opcode, int count, bool nowait)
{
	struct mx_sg_context *sg_ctx;
	struct mx_transfer **transfers;
	ssize_t ret;

	sg_ctx = mx_sg_context_create(mx_pdev, buf, size, dir);
	if (IS_ERR(sg_ctx))
		return PTR_ERR(sg_ctx);

	if (count == 1) {
		struct mx_transfer *transfer;

		transfer = alloc_mx_transfer_sg(sg_ctx, 0, size, device_addr);
		mx_sg_context_put(sg_ctx);
		if (!transfer) {
			pr_warn("Failed to alloc mx_transfer\n");
			return -ENOMEM;
		}
		return mx_transfer_submit_sg_one(ctx, mx_pdev, transfer, opcode, nowait);
	}

	transfers = alloc_mx_transfers(sg_ctx, device_addr, count);
	mx_sg_context_put(sg_ctx);
	if (!transfers) {
		pr_warn("Failed to alloc parallel mx_transfers (count=%d)\n", count);
		return -ENOMEM;
	}

	ret = mx_transfer_submit_sg_parallel(ctx, mx_pdev, transfers, opcode, count, nowait);
	return ret;
}

static int mx_transfer_init_ctrl(struct mx_file_ctx *ctx,
		struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer, int opcode)
{
	int ret = mx_lease_transfer_get(ctx, transfer);

	if (ret)
		return ret;
	transfer->command = mx_pdev->ops.create_command_ctrl(transfer, opcode);
	if (IS_ERR_OR_NULL(transfer->command)) {
		int ret = transfer->command ? PTR_ERR(transfer->command) : -ENOMEM;

		transfer->command = NULL;
		return ret;
	}

	transfer->mx_pdev = mx_pdev;
	transfer->is_sg = false;
	INIT_WORK(&transfer->work, mx_transfer_wait_work);

	return 0;
}

static int mx_transfer_destroy_ctrl(struct mx_transfer *transfer)
{
	int ret = 0;

	if (transfer->dir == DMA_FROM_DEVICE) {
		if (access_ok(transfer->user_addr, transfer->size)) {
			ret = copy_to_user(transfer->user_addr, &transfer->result, sizeof(uint64_t));
			if (ret)
				pr_warn("Failed to copy_to_user (id=%u, %llx -> %llx, err=%d)\n",
						transfer->id, (uint64_t)&transfer->result, (uint64_t)transfer->user_addr, ret);
		} else {
			*(uint64_t *)transfer->user_addr = transfer->result;
		}
	}

	release_mx_transfer(transfer);

	return ret;
}

static ssize_t mx_transfer_submit_ctrl(struct mx_file_ctx *ctx,
		struct mx_pci_dev *mx_pdev,
		struct mx_transfer *transfer, int opcode, bool nowait)
{
	size_t size = transfer->size;
	ssize_t ret;

	ret = mx_transfer_init_ctrl(ctx, mx_pdev, transfer, opcode);
	if (ret < 0) {
		release_mx_transfer(transfer);
		return ret;
	}

	trace_mx_dma_xfer_enqueue(mx_pdev->dev_id, transfer->id, opcode,
			transfer->dir, transfer->size, false, 0, 1);
	mx_transfer_queue(mx_pdev->io_queue, transfer);
	if (nowait) {
		percpu_ref_get(&mx_pdev->io_ref);

		mx_transfer_schedule_wait(mx_pdev, transfer);
		return size;
	}

	return mx_transfer_wait(mx_pdev, transfer);
}

static int mx_parallel_count_for(struct mx_pci_dev *mx_pdev, void __user *buf, size_t size)
{
	uintptr_t first_page = (uintptr_t)buf >> PAGE_SHIFT;
	uintptr_t last_page = ((uintptr_t)buf + size - 1) >> PAGE_SHIFT;
	size_t nr_pages_sz = last_page - first_page + 1;
	int nr_pages = (int)min_t(size_t, nr_pages_sz, INT_MAX);
	size_t dma_size, total_descs, raw_count;
	int descs_per_list, descs_per_split;
	int count;

	if (parallel_split_ratio == 0) {
		count = min_t(unsigned int, nr_pages, parallel_count);
		return max_t(int, count, 1);
	}

	dma_size = mx_pdev->page_size;
	descs_per_list = (int)(dma_size / sizeof(uint64_t));
	descs_per_split = descs_per_list * parallel_split_ratio / 100;
	if (descs_per_split < 1)
		descs_per_split = 1;

	total_descs = DIV_ROUND_UP(size, dma_size);
	raw_count = DIV_ROUND_UP(total_descs, (size_t)descs_per_split);
	count = (int)min_t(size_t, raw_count, INT_MAX);
	if (count < 1)
		count = 1;

	count = min_t(int, count, nr_pages);
	count = min_t(unsigned int, count, parallel_count);
	return max_t(int, count, 1);
}

ssize_t read_data_from_device(struct mx_file_ctx *ctx,
		char __user *user_addr, size_t size, loff_t *fpos, int opcode)
{
	struct mx_pci_dev *mx_pdev = ctx->mx_pdev;

	return mx_transfer_submit_sg_split(ctx, mx_pdev, user_addr, size, *fpos,
			DMA_FROM_DEVICE, opcode, 1, false);
}

ssize_t write_data_to_device(struct mx_file_ctx *ctx,
		const char __user *user_addr, size_t size, loff_t *fpos, int opcode, bool nowait)
{
	struct mx_pci_dev *mx_pdev = ctx->mx_pdev;

	return mx_transfer_submit_sg_split(ctx, mx_pdev, (void __user *)user_addr, size, *fpos,
			DMA_TO_DEVICE, opcode, 1, nowait);
}

ssize_t read_data_from_device_parallel(struct mx_file_ctx *ctx,
		char __user *buf, size_t size, loff_t *fpos, int opcode)
{
	struct mx_pci_dev *mx_pdev = ctx->mx_pdev;
	int count = mx_parallel_count_for(mx_pdev, buf, size);

	return mx_transfer_submit_sg_split(ctx, mx_pdev, buf, size, *fpos,
			DMA_FROM_DEVICE, opcode, count, false);
}

ssize_t write_data_to_device_parallel(struct mx_file_ctx *ctx,
		const char __user *buf, size_t size, loff_t *fpos, int opcode, bool nowait)
{
	struct mx_pci_dev *mx_pdev = ctx->mx_pdev;
	int count = mx_parallel_count_for(mx_pdev, (void __user *)buf, size);

	return mx_transfer_submit_sg_split(ctx, mx_pdev, (void __user *)buf, size, *fpos,
			DMA_TO_DEVICE, opcode, count, nowait);
}

ssize_t read_ctrl_from_device(struct mx_file_ctx *ctx,
		char __user *user_addr, size_t size, loff_t *fpos, int opcode)
{
	struct mx_pci_dev *mx_pdev = ctx->mx_pdev;
	struct mx_transfer *transfer;

	transfer = alloc_mx_transfer(user_addr, size, *fpos, DMA_FROM_DEVICE);
	if (!transfer) {
		pr_warn("Failed to alloc mx_transfer for read_ctrl\n");
		return -ENOMEM;
	}

	return mx_transfer_submit_ctrl(ctx, mx_pdev, transfer, opcode, false);
}

ssize_t write_ctrl_to_device(struct mx_file_ctx *ctx,
		const char __user *user_addr, size_t size, loff_t *fpos, int opcode, bool nowait)
{
	struct mx_pci_dev *mx_pdev = ctx->mx_pdev;
	struct mx_transfer *transfer;

	transfer = alloc_mx_transfer((void __user *)user_addr, size, *fpos, DMA_TO_DEVICE);
	if (!transfer) {
		pr_warn("Failed to alloc mx_transfer for write_ctrl\n");
		return -ENOMEM;
	}

	return mx_transfer_submit_ctrl(ctx, mx_pdev, transfer, opcode, nowait);
}

ssize_t submit_protocol_transfer(struct mx_file_ctx *ctx, char __user *buf,
		size_t size, int opcode)
{
	struct mx_pci_dev *mx_pdev = ctx->mx_pdev;
	/* INTENTIONAL: HIO send and recv each DMA both ways on this one buffer. */
	return mx_transfer_submit_sg_split(ctx, mx_pdev, buf, size, 0,
			DMA_BIDIRECTIONAL, opcode, 1, false);
}

long submit_passthru_command(struct mx_file_ctx *ctx, int subopcode,
			     uint64_t device_addr, uint64_t size, bool no_completion,
			     uint8_t *out_status, uint64_t *out_host_addr)
{
	struct mx_pci_dev *mx_pdev = ctx->mx_pdev;
	struct mx_transfer *transfer;
	long left_time;

	if (!mx_pdev->ops.create_command_passthru)
		return -EOPNOTSUPP;

	transfer = alloc_mx_transfer(NULL, size, device_addr, DMA_NONE);
	if (!transfer)
		return -ENOMEM;
	{
		int ret = mx_lease_transfer_get(ctx, transfer);

		if (ret) {
			release_mx_transfer(transfer);
			return ret;
		}
	}

	transfer->command = mx_pdev->ops.create_command_passthru(transfer, subopcode);
	if (!transfer->command) {
		release_mx_transfer(transfer);
		return -ENOMEM;
	}

	transfer->mx_pdev = mx_pdev;
	transfer->is_sg = false;
	transfer->no_completion = no_completion;
	INIT_WORK(&transfer->work, mx_transfer_wait_work);

	trace_mx_dma_xfer_enqueue(mx_pdev->dev_id, transfer->id, IO_OPCODE_PASSTHRU,
			transfer->dir, transfer->size, false, 0, 1);
	mx_transfer_queue(mx_pdev->io_queue, transfer);

	if (no_completion) {
		left_time = wait_for_completion_interruptible_timeout(&transfer->done,
				msecs_to_jiffies(timeout_ms));
		if (left_time <= 0) {
			long ret = (left_time == 0) ? -ETIMEDOUT : -EINTR;

			if (!mx_transfer_remove_from_sq(mx_pdev->io_queue, transfer))
				wait_for_completion(&transfer->done);
			release_mx_transfer(transfer);
			return ret;
		}
		release_mx_transfer(transfer);
		return 0;
	}

	left_time = wait_for_completion_interruptible_timeout(&transfer->done, msecs_to_jiffies(timeout_ms));

	if (left_time <= 0) {
		unsigned long flags;
		long ret = (left_time == 0) ? -ETIMEDOUT : -EINTR;

		if (left_time == 0)
			pr_warn("passthru command timeout (subopcode=%d, timeout=%u ms)\n",
				subopcode, timeout_ms);
		else
			pr_warn("passthru command interrupted (subopcode=%d)\n", subopcode);

		if (mx_transfer_remove_from_sq(mx_pdev->io_queue, transfer)) {
			release_mx_transfer(transfer);
			return ret;
		}

		WRITE_ONCE(transfer->is_zombie, true);
		transfer->zombie_timestamp = jiffies;
		atomic_inc(&mx_pdev->io_queue->zombie_wait_count);

		spin_lock_irqsave(&mx_pdev->zombie_lock, flags);
		list_add_tail(&transfer->zombie_entry, &mx_pdev->zombie_list);
		spin_unlock_irqrestore(&mx_pdev->zombie_lock, flags);
		return ret;
	}

	if (out_host_addr)
		*out_host_addr = transfer->result;
	if (out_status)
		*out_status = transfer->status;

	release_mx_transfer(transfer);
	return 0;
}

static void drain_zombie_list(struct mx_pci_dev *mx_pdev, struct list_head *list)
{
	struct mx_transfer *transfer, *tmp;
	bool drained = !list_empty(list);

	list_for_each_entry_safe(transfer, tmp, list, zombie_entry) {
		bool claimed = (atomic_cmpxchg(&transfer->wait_claimed, 0, 1) == 0);

		list_del(&transfer->zombie_entry);

		if (claimed)
			atomic_dec(&mx_pdev->io_queue->wait_count);
		atomic_dec(&mx_pdev->io_queue->zombie_wait_count);

		cancel_work_sync(&transfer->work);

		if (transfer->is_sg)
			desc_list_free(mx_pdev, transfer);

		release_mx_transfer(transfer);
	}

	if (drained)
		swake_up_one(&mx_pdev->io_queue->sq_wait);
}

int zombie_cleanup_handler(void *data)
{
	struct mx_pci_dev *mx_pdev = data;
	struct mx_transfer *transfer, *tmp;
	unsigned long flags;
	LIST_HEAD(to_cleanup);

	while (!kthread_should_stop()) {
		msleep_interruptible(1000);

		if (kthread_should_stop())
			break;

		spin_lock_irqsave(&mx_pdev->zombie_lock, flags);
		list_for_each_entry_safe(transfer, tmp, &mx_pdev->zombie_list, zombie_entry) {
			bool hw_done = (atomic_read(&transfer->wait_claimed) == 1);

			if (hw_done) {
				list_del(&transfer->zombie_entry);
				list_add_tail(&transfer->zombie_entry, &to_cleanup);
			}
		}
		spin_unlock_irqrestore(&mx_pdev->zombie_lock, flags);

		drain_zombie_list(mx_pdev, &to_cleanup);
	}

	if (!READ_ONCE(mx_pdev->dma_reclaim_safe))
		return 0;
	spin_lock_irqsave(&mx_pdev->zombie_lock, flags);
	list_splice_init(&mx_pdev->zombie_list, &to_cleanup);
	spin_unlock_irqrestore(&mx_pdev->zombie_lock, flags);

	drain_zombie_list(mx_pdev, &to_cleanup);

	return 0;
}

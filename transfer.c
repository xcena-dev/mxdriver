// SPDX-License-Identifier: <SPDX License Expression>

#include "mx_dma.h"

#ifndef MX_DMA_DISABLE_TRACE
#include "trace.h"
#else
#define trace_mx_dma_xfer_enqueue(dev_id, xfer_id, op, dir, sz, sg, pi, pc)	do { } while (0)
#define trace_mx_dma_xfer_wait_exit(dev_id, xfer_id, ret, state)		do { } while (0)
#endif

/* mx_transfer_wait terminal state. Keep in sync with mx_dma_wait_state_names
 * in trace.h. */
enum mx_dma_wait_state {
	MX_DMA_WAIT_COMPLETED		= 0,
	MX_DMA_WAIT_RECOVERED		= 1,
	MX_DMA_WAIT_LATE_COMPLETED	= 2,
	MX_DMA_WAIT_ZOMBIE		= 3,
};

/* Keep MX_DMA_WAIT_* in sync with mx_dma_wait_state_names in trace.h. */
static_assert(MX_DMA_WAIT_COMPLETED      == 0, "trace wait_state names out of sync");
static_assert(MX_DMA_WAIT_RECOVERED      == 1, "trace wait_state names out of sync");
static_assert(MX_DMA_WAIT_LATE_COMPLETED == 2, "trace wait_state names out of sync");
static_assert(MX_DMA_WAIT_ZOMBIE         == 3, "trace wait_state names out of sync");

unsigned int timeout_ms = 60000; /* 60 seconds */
module_param(timeout_ms, int, 0644);
unsigned int parallel_count = 6;
module_param(parallel_count, int, 0644);
/*
 * parallel_split_ratio: split granularity as % of one PRP list
 * (descs_per_list = page_size / sizeof(u64) — 128 on v1, 512 on v2).
 *     0 : legacy per-page split, capped at parallel_count (A/B with main)
 *    50 : default; ~2× splits of ratio=100, empirically best
 *   100 : one split per full descriptor list, no chain at boundary
 *   200 : chain across two lists, half as many splits
 * Sysfs-writable (0644); applies to subsequently submitted transfers only.
 */
unsigned int parallel_split_ratio = 50;
module_param(parallel_split_ratio, uint, 0644);
unsigned int zombie_grace_ms = 60000; /* 60 seconds, 0=immediate */
module_param(zombie_grace_ms, int, 0644);

/******************************************************************************/
/* Shared SG context (pin + dma_map_sg done once, shared across split-transfers) */
/******************************************************************************/
static void mx_sg_context_release(struct mx_sg_context *ctx)
{
	struct device *dev = &ctx->mx_pdev->pdev->dev;
	struct sg_table *sgt = &ctx->sgt;
	int i;

	if (sgt->nents)
		dma_unmap_sg(dev, sgt->sgl, sgt->orig_nents, ctx->dir);

	if (ctx->dir != DMA_TO_DEVICE) {
		for (i = 0; i < ctx->pages_nr; i++)
			set_page_dirty_lock(ctx->pages[i]);
	}

	if (ctx->pages_nr > 0)
		unpin_user_pages(ctx->pages, ctx->pages_nr);

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

/*
 * Create a shared SG mapping for a user buffer; splits attach via mx_sg_context_get/put.
 * Caller owns the initial refcount.  Returns ERR_PTR on failure so callers can distinguish
 * -EFAULT (bad addr) / -EIO (dma_map) / -ENOMEM (alloc); partial state is freed via put.
 */
static struct mx_sg_context *mx_sg_context_create(struct mx_pci_dev *mx_pdev,
		void __user *user_addr, size_t total_size, enum dma_data_direction dir)
{
	struct mx_sg_context *ctx;
	struct sg_table *sgt;
	unsigned int pages_nr, offset, gup_flags = 0;
	long pinned;
	int ret;

	offset = offset_in_page((unsigned long)user_addr);
	pages_nr = DIV_ROUND_UP(offset + total_size, PAGE_SIZE);
	if (!pages_nr)
		return ERR_PTR(-EINVAL);

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
		/* kvmalloc_array: vmalloc fallback past KMALLOC_MAX_SIZE.  Pages array is walked,
		 * not DMA'd, so physical contiguity is unneeded — removes the ~2 GB per-ioctl cap. */
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
		ctx->pages_nr = pinned; /* tracked so release can unpin */
	if (pinned != pages_nr) {
		const char *kind = pinned < 0 ? "failed" :
				   pinned == 0 ? "none" : "partial";

		pr_warn("pin_user_pages_fast %s (req=%u, got=%ld)\n", kind, pages_nr, pinned);
		ret = (pinned < 0) ? (int)pinned : -EFAULT;
		goto err;
	}

	if (pages_nr <= MX_PAGES_INLINE_NR) {
		/* Hand-build single-entry sg_table on the inline scatterlist. */
		sg_init_table(ctx->sg_inline, MX_PAGES_INLINE_NR);
		sg_set_page(&ctx->sg_inline[0], ctx->pages[0], total_size, offset);
		sgt->sgl = ctx->sg_inline;
		sgt->orig_nents = pages_nr;
	} else {
		ret = sg_alloc_table_from_pages(sgt, ctx->pages, pages_nr, offset, total_size, GFP_KERNEL);
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

/******************************************************************************/
/* MX Transfer                                                                */
/******************************************************************************/
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

/*
 * Inline-aware free of the hardware command buffer plus the mx_transfer slab entry.
 * Centralised so release_mx_transfer() and drain_zombie_list() cannot drift on the cmd_inline identity check —
 * divergence here would be a use-after-free or double-free in a kernel path.
 */
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
	transfer_id_free(transfer->id);
	free_mx_transfer(transfer);
}

/* Base allocator — used directly by ctrl / passthru paths (no SG mapping). */
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

/* SG split-transfer attaching to an existing shared sg_context.  Takes one ref on sg_ctx (released
 * when the transfer finishes via destroy_sg / release_mx_transfer).  SG path never reads
 * transfer->user_addr (consumers use sg_ctx->user_addr + sg_byte_offset), so it is left NULL. */
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

/*
 * Build `count` split-transfers sharing one sg_context.  Caller's initial
 * sg_ctx reference must be put() after this returns; lifetime then follows
 * the split-transfers.  Splits are page-aligned except for the head (which
 * inherits the buffer's intra-page offset) and the tail (trailing partial page).
 */
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
	unsigned long flags;

	init_completion(&transfer->done);

	spin_lock_irqsave(&queue->sq_lock, flags);
	list_add_tail(&transfer->entry, &queue->sq_list);
	spin_unlock_irqrestore(&queue->sq_lock, flags);
	swake_up_one(&queue->sq_wait);
}

static void mx_transfer_queue_parallel(struct mx_queue *queue, struct mx_transfer **transfers, int count)
{
	unsigned long flags;
	int i;

	for (i = 0; i < count; i++)
		init_completion(&transfers[i]->done);

	spin_lock_irqsave(&queue->sq_lock, flags);
	for (i = 0; i < count; i++)
		list_add_tail(&transfers[i]->entry, &queue->sq_list);
	spin_unlock_irqrestore(&queue->sq_lock, flags);
	swake_up_one(&queue->sq_wait);
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
static ssize_t mx_transfer_wait(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer)
{
	unsigned long left_time;
	ssize_t size;
	ssize_t ret;
	int state;
	/* Capture id up-front: destroy/release below frees the transfer. */
	u32 __maybe_unused xfer_id = (u32)transfer->id;

	left_time = wait_for_completion_interruptible_timeout(&transfer->done, msecs_to_jiffies(timeout_ms));
	if ((long)left_time <= 0) {
		unsigned long flags;

		if (left_time == 0)
			pr_warn("wait_for_completion is timeout (id=%u, size=%#llx, dir=%u, timeout=%u ms)\n",
					transfer->id, (uint64_t)transfer->size, transfer->dir, timeout_ms);
		else
			pr_warn("wait_for_completion is interrupted (id=%u, size=%#llx, dir=%u)\n",
					transfer->id, (uint64_t)transfer->size, transfer->dir);

		if (mx_transfer_remove_from_sq(mx_pdev->io_queue, transfer)) {
			if (transfer->is_sg)
				mx_transfer_destroy_sg(mx_pdev, transfer);
			else
				release_mx_transfer(transfer);
			ret = 0;
			state = MX_DMA_WAIT_RECOVERED;
			goto out;
		}

		/*
		 * Transfer already submitted to HW. Wait non-interruptibly for
		 * completion to avoid leaving the CXL transport in a corrupt
		 * state. A short timeout prevents hanging process exit.
		 */
		left_time = wait_for_completion_timeout(&transfer->done,
							msecs_to_jiffies(1000));
		if (left_time > 0) {
			/* Completed — clean up normally */
			size = transfer->size;
			if (transfer->is_sg)
				mx_transfer_destroy_sg(mx_pdev, transfer);
			else
				mx_transfer_destroy_ctrl(transfer);
			ret = size;
			state = MX_DMA_WAIT_LATE_COMPLETED;
			goto out;
		}

		pr_warn("mx_dma: interrupted transfer did not complete within 1s (id=%u), marking zombie\n",
				transfer->id);

		/* Mark as zombie - device might still be accessing memory */
		WRITE_ONCE(transfer->is_zombie, true);
		transfer->zombie_timestamp = jiffies;
		atomic_inc(&mx_pdev->io_queue->zombie_wait_count);

		spin_lock_irqsave(&mx_pdev->zombie_lock, flags);
		list_add_tail(&transfer->zombie_entry, &mx_pdev->zombie_list);
		spin_unlock_irqrestore(&mx_pdev->zombie_lock, flags);

		ret = 0;
		state = MX_DMA_WAIT_ZOMBIE;
		goto out;
	}

	size = transfer->size;

	if (transfer->is_sg)
		mx_transfer_destroy_sg(mx_pdev, transfer);
	else
		mx_transfer_destroy_ctrl(transfer);

	ret = size;
	state = MX_DMA_WAIT_COMPLETED;
out:
	trace_mx_dma_xfer_wait_exit(mx_pdev->dev_id, xfer_id, ret, state);
	return ret;
}

static void mx_transfer_wait_work(struct work_struct *work)
{
	struct mx_transfer *transfer = container_of(work, struct mx_transfer, work);
	struct mx_pci_dev *mx_pdev = transfer->mx_pdev;

	mx_transfer_wait(mx_pdev, transfer);
}

static int mx_transfer_init_sg(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer, int opcode)
{
	transfer->command = mx_pdev->ops.create_command_sg(mx_pdev, transfer, opcode);
	if (!transfer->command) {
		pr_warn("Failed to create_command_sg (id=%u)\n", transfer->id);
		return -ENOMEM;
	}

	transfer->mx_pdev = mx_pdev;
	transfer->is_sg = true;
	INIT_WORK(&transfer->work, mx_transfer_wait_work);

	return 0;
}

static void mx_transfer_destroy_sg(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer)
{
	desc_list_free(mx_pdev, transfer);
	release_mx_transfer(transfer); /* drops sg_ctx reference */
}

static ssize_t mx_transfer_submit_sg_one(struct mx_pci_dev *mx_pdev,
		struct mx_transfer *transfer, int opcode, bool nowait)
{
	size_t size = transfer->size;
	ssize_t ret;

	ret = mx_transfer_init_sg(mx_pdev, transfer, opcode);
	if (ret < 0) {
		release_mx_transfer(transfer);
		return ret;
	}

	trace_mx_dma_xfer_enqueue(mx_pdev->dev_id, transfer->id, opcode,
			transfer->dir, transfer->size, true, 0, 1);
	mx_transfer_queue(mx_pdev->io_queue, transfer);
	if (nowait) {
		schedule_work(&transfer->work);
		return size;
	}

	return mx_transfer_wait(mx_pdev, transfer);
}

static ssize_t mx_transfer_submit_sg_parallel(struct mx_pci_dev *mx_pdev,
		struct mx_transfer **transfers, int opcode, int count, bool nowait)
{
	int initialized_count = 0;
	ssize_t transferred = 0;
	size_t total_size = 0;
	int ret = 0;
	int i;

	for (i = 0; i < count; i++) {
		ret = mx_transfer_init_sg(mx_pdev, transfers[i], opcode);
		if (ret < 0)
			break;
		total_size += transfers[i]->size;
		initialized_count++;
	}

	if (ret < 0) {
		for (i = 0; i < initialized_count; i++)
			mx_transfer_destroy_sg(mx_pdev, transfers[i]);

		for (i = initialized_count; i < count; i++)
			release_mx_transfer(transfers[i]);

		kfree(transfers);
		return ret;
	}

	for (i = 0; i < count; i++)
		trace_mx_dma_xfer_enqueue(mx_pdev->dev_id, transfers[i]->id, opcode,
				transfers[i]->dir, transfers[i]->size, true, i, count);

	mx_transfer_queue_parallel(mx_pdev->io_queue, transfers, count);

	if (nowait) {
		for (i = 0; i < count; i++)
			schedule_work(&transfers[i]->work);
		kfree(transfers);
		return total_size;
	}

	for (i = 0; i < count; i++)
		transferred += mx_transfer_wait(mx_pdev, transfers[i]);

	kfree(transfers);
	return transferred;
}

/*
 * Submit a (possibly split) SG data transfer.  Even when count==1 we wrap the
 * buffer in a shared sg_context so single and parallel paths share the same
 * destroy / zombie semantics.
 */
static ssize_t mx_transfer_submit_sg_split(struct mx_pci_dev *mx_pdev,
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
		mx_sg_context_put(sg_ctx); /* drop creator ref; transfer owns one now */
		if (!transfer) {
			pr_warn("Failed to alloc mx_transfer\n");
			return -ENOMEM;
		}
		return mx_transfer_submit_sg_one(mx_pdev, transfer, opcode, nowait);
	}

	transfers = alloc_mx_transfers(sg_ctx, device_addr, count);
	mx_sg_context_put(sg_ctx); /* drop creator ref; splits own their refs */
	if (!transfers) {
		pr_warn("Failed to alloc parallel mx_transfers (count=%d)\n", count);
		return -ENOMEM;
	}

	ret = mx_transfer_submit_sg_parallel(mx_pdev, transfers, opcode, count, nowait);
	return ret;
}

static int mx_transfer_init_ctrl(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer, int opcode)
{
	transfer->command = mx_pdev->ops.create_command_ctrl(transfer, opcode);
	if (!transfer->command)
		return -ENOMEM;

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

static ssize_t mx_transfer_submit_ctrl(struct mx_pci_dev *mx_pdev,
		struct mx_transfer *transfer, int opcode, bool nowait)
{
	size_t size = transfer->size;
	ssize_t ret;

	ret = mx_transfer_init_ctrl(mx_pdev, transfer, opcode);
	if (ret < 0) {
		release_mx_transfer(transfer);
		return ret;
	}

	trace_mx_dma_xfer_enqueue(mx_pdev->dev_id, transfer->id, opcode,
			transfer->dir, transfer->size, false, 0, 1);
	mx_transfer_queue(mx_pdev->io_queue, transfer);
	if (nowait) {
		schedule_work(&transfer->work);
		return size;
	}

	return mx_transfer_wait(mx_pdev, transfer);
}

/******************************************************************************/
/* Functions for fops                                                         */
/******************************************************************************/

/*
 * Decide how many splits to break a buffer into.
 *   ratio == 0 : legacy page-based, count = nr_pages capped at parallel_count (A/B with main).
 *   ratio  > 0 : descriptor-based using
 *     dma_size        = mx_pdev->page_size                  (PRP entry granularity)
 *     descs_per_list  = dma_size / sizeof(u64)              (v1: 128, v2: 512)
 *     descs_per_split = descs_per_list * ratio / 100        (one split's PRP capacity)
 *     total_descs     = ceil(size / dma_size)
 *     count           = ceil(total_descs / descs_per_split), then capped at parallel_count and nr_pages.
 */
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
		count = min_t(int, nr_pages, parallel_count);
		return max_t(int, count, 1);
	}

	dma_size = mx_pdev->page_size;
	descs_per_list = (int)(dma_size / sizeof(uint64_t));
	descs_per_split = descs_per_list * parallel_split_ratio / 100;
	if (descs_per_split < 1)
		descs_per_split = 1;

	/* size_t math + INT_MAX clamp: v1 (dma_size=1024) would overflow int total_descs at size > 2 TiB
	 * (size_t intermediate lifts that ceiling to the pin_user_pages_fast int-nr_pages limit, ~8 TiB). */
	total_descs = DIV_ROUND_UP(size, dma_size);
	raw_count = DIV_ROUND_UP(total_descs, (size_t)descs_per_split);
	count = (int)min_t(size_t, raw_count, INT_MAX);
	if (count < 1)
		count = 1;

	/* Clamp upper to nr_pages: alloc_mx_transfers splits by pages, so count > nr_pages would yield
	 * zero-byte splits and submit undefined DMA.  Lower clamp to 1 guards parallel_count=0 div-by-zero. */
	count = min_t(int, count, nr_pages);
	count = min_t(int, count, parallel_count);
	return max_t(int, count, 1);
}

ssize_t read_data_from_device(struct mx_pci_dev *mx_pdev,
		char __user *user_addr, size_t size, loff_t *fpos, int opcode)
{
	return mx_transfer_submit_sg_split(mx_pdev, user_addr, size, *fpos,
			DMA_FROM_DEVICE, opcode, 1, false);
}

ssize_t write_data_to_device(struct mx_pci_dev *mx_pdev,
		const char __user *user_addr, size_t size, loff_t *fpos, int opcode, bool nowait)
{
	return mx_transfer_submit_sg_split(mx_pdev, (void __user *)user_addr, size, *fpos,
			DMA_TO_DEVICE, opcode, 1, nowait);
}

ssize_t read_data_from_device_parallel(struct mx_pci_dev *mx_pdev,
		char __user *buf, size_t size, loff_t *fpos, int opcode)
{
	int count = mx_parallel_count_for(mx_pdev, buf, size);

	return mx_transfer_submit_sg_split(mx_pdev, buf, size, *fpos,
			DMA_FROM_DEVICE, opcode, count, false);
}

ssize_t write_data_to_device_parallel(struct mx_pci_dev *mx_pdev,
		const char __user *buf, size_t size, loff_t *fpos, int opcode, bool nowait)
{
	int count = mx_parallel_count_for(mx_pdev, (void __user *)buf, size);

	return mx_transfer_submit_sg_split(mx_pdev, (void __user *)buf, size, *fpos,
			DMA_TO_DEVICE, opcode, count, nowait);
}

ssize_t read_ctrl_from_device(struct mx_pci_dev *mx_pdev,
		char __user *user_addr, size_t size, loff_t *fpos, int opcode)
{
	struct mx_transfer *transfer;

	transfer = alloc_mx_transfer(user_addr, size, *fpos, DMA_FROM_DEVICE);
	if (!transfer) {
		pr_warn("Failed to alloc mx_transfer for read_ctrl\n");
		return -ENOMEM;
	}

	return mx_transfer_submit_ctrl(mx_pdev, transfer, opcode, false);
}

ssize_t write_ctrl_to_device(struct mx_pci_dev *mx_pdev,
		const char __user *user_addr, size_t size, loff_t *fpos, int opcode, bool nowait)
{
	struct mx_transfer *transfer;

	transfer = alloc_mx_transfer((void __user *)user_addr, size, *fpos, DMA_TO_DEVICE);
	if (!transfer) {
		pr_warn("Failed to alloc mx_transfer for write_ctrl\n");
		return -ENOMEM;
	}

	return mx_transfer_submit_ctrl(mx_pdev, transfer, opcode, nowait);
}

/******************************************************************************/
/* Protocol transfer (HIO Send/Recv)                                          */
/******************************************************************************/
ssize_t submit_protocol_transfer(struct mx_pci_dev *mx_pdev, char __user *buf, size_t size, int opcode)
{
	/*
	 * HIO Send/Recv both perform H2D and D2H on the same host buffer:
	 *   Send: H2D(full buffer) -> firmware -> D2H(status in command page)
	 *   Recv: H2D(command page) -> firmware -> D2H(response in full buffer)
	 * DMA_BIDIRECTIONAL is required.  Treated as a single transfer (no split).
	 */
	return mx_transfer_submit_sg_split(mx_pdev, buf, size, 0,
			DMA_BIDIRECTIONAL, opcode, 1, false);
}

/******************************************************************************/
/* Passthrough command                                                        */
/******************************************************************************/
long submit_passthru_command(struct mx_pci_dev *mx_pdev, int subopcode,
			     uint64_t device_addr, uint64_t size, bool no_completion,
			     uint8_t *out_status, uint64_t *out_host_addr)
{
	struct mx_transfer *transfer;
	unsigned long left_time;

	if (!mx_pdev->ops.create_command_passthru)
		return -EOPNOTSUPP;

	transfer = alloc_mx_transfer(NULL, size, device_addr, DMA_NONE);
	if (!transfer)
		return -ENOMEM;

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
		/*
		 * HW guarantees no completion for this command.  The submit
		 * handler signals transfer->done once the command is pushed,
		 * so we only wait for the push — no timeout/zombie handling.
		 */
		wait_for_completion(&transfer->done);
		release_mx_transfer(transfer);
		return 0;
	}

	left_time = wait_for_completion_interruptible_timeout(&transfer->done, msecs_to_jiffies(timeout_ms));

	if ((long)left_time <= 0) {
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

/******************************************************************************/
/* Zombie Transfer Cleanup                                                   */
/******************************************************************************/
static void drain_zombie_list(struct mx_pci_dev *mx_pdev, struct list_head *list)
{
	struct mx_transfer *transfer, *tmp;

	list_for_each_entry_safe(transfer, tmp, list, zombie_entry) {
		bool claimed = (atomic_cmpxchg(&transfer->wait_claimed, 0, 1) == 0);

		list_del(&transfer->zombie_entry);

		if (claimed)
			atomic_dec(&mx_pdev->io_queue->wait_count);
		atomic_dec(&mx_pdev->io_queue->zombie_wait_count);

		cancel_work_sync(&transfer->work);

		if (transfer->is_sg)
			desc_list_free(mx_pdev, transfer);

		/* release_mx_transfer drops the sg_context ref (if any), frees the IDR id, and the slab entry.
		 * Last split on an sg_context triggers dma_unmap_sg + unpin_user_pages on its put(). */
		release_mx_transfer(transfer);
	}
}

int zombie_cleanup_handler(void *data)
{
	struct mx_pci_dev *mx_pdev = data;
	struct mx_transfer *transfer, *tmp;
	unsigned long flags;
	LIST_HEAD(to_cleanup);

	while (!kthread_should_stop()) {
		unsigned long grace = msecs_to_jiffies(zombie_grace_ms);

		msleep_interruptible(1000);

		if (kthread_should_stop())
			break;

		/* Collect zombies ready for cleanup */
		spin_lock_irqsave(&mx_pdev->zombie_lock, flags);
		list_for_each_entry_safe(transfer, tmp, &mx_pdev->zombie_list, zombie_entry) {
			bool hw_done = (atomic_read(&transfer->wait_claimed) == 1);

			if (hw_done || time_after(jiffies,
					transfer->zombie_timestamp + grace)) {
				list_del(&transfer->zombie_entry);
				list_add_tail(&transfer->zombie_entry, &to_cleanup);
			}
		}
		spin_unlock_irqrestore(&mx_pdev->zombie_lock, flags);

		drain_zombie_list(mx_pdev, &to_cleanup);
	}

	/* Final drain: force-clean all remaining zombies on thread exit */
	spin_lock_irqsave(&mx_pdev->zombie_lock, flags);
	list_splice_init(&mx_pdev->zombie_list, &to_cleanup);
	spin_unlock_irqrestore(&mx_pdev->zombie_lock, flags);

	drain_zombie_list(mx_pdev, &to_cleanup);

	return 0;
}

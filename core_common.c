// SPDX-License-Identifier: <SPDX License Expression>

#include "mx_dma.h"

#ifndef MX_DMA_DISABLE_TRACE
#include "trace.h"
#else
#define trace_mx_dma_xfer_submit(xfer_id, no_completion)			do { } while (0)
#define trace_mx_dma_xfer_post_submit(dev_id, pushed_count)			do { } while (0)
#define trace_mx_dma_xfer_complete(xfer_id, status, result, is_zombie)		do { } while (0)
#define trace_mx_dma_xfer_complete_orphan(xfer_id, status, result)		do { } while (0)
#endif

int mx_get_list_count(size_t total_desc_cnt, int descs_per_list)
{
	int list_cnt = 1;

	while (total_desc_cnt > descs_per_list) {
		total_desc_cnt -= (descs_per_list - 1);
		list_cnt++;
	}

	return list_cnt;
}

int mx_sg_locate(struct sg_table *sgt, size_t byte_offset,
		 struct scatterlist **out_sg, size_t *out_intra)
{
	struct scatterlist *sg;
	size_t acc = 0;
	int i;

	for_each_sgtable_dma_sg(sgt, sg, i) {
		size_t dlen = sg_dma_len(sg);

		if (acc + dlen > byte_offset) {
			*out_sg = sg;
			*out_intra = byte_offset - acc;
			return 0;
		}
		acc += dlen;
	}

	*out_sg = NULL;
	*out_intra = 0;
	return -EINVAL;
}

static size_t prp_chunk_len_at(dma_addr_t dma_addr, size_t dma_size)
{
	size_t rem = (size_t)(dma_addr & (dma_size - 1));

	return rem ? (dma_size - rem) : dma_size;
}

size_t mx_prp_first_chunk_len(struct scatterlist *sg, size_t intra_off, size_t dma_size)
{
	size_t len = prp_chunk_len_at(sg_dma_address(sg) + intra_off, dma_size);

	return min_t(size_t, len, sg_dma_len(sg) - intra_off);
}

int mx_get_total_desc_count(struct scatterlist *sg, size_t intra_off, size_t byte_size,
			    size_t dma_size, size_t *out_cnt)
{
	size_t remaining = byte_size;
	size_t total = 0;
	dma_addr_t end;

	*out_cnt = 0;
	if (byte_size == 0)
		return -EINVAL;

	while (remaining > 0 && sg) {
		size_t avail = sg_dma_len(sg) - intra_off;
		size_t consumed = min(avail, remaining);
		size_t first_len = mx_prp_first_chunk_len(sg, intra_off, dma_size);

		first_len = min(first_len, consumed);
		total += 1;
		if (consumed > first_len)
			total += DIV_ROUND_UP(consumed - first_len, dma_size);

		remaining -= consumed;
		if (remaining == 0)
			break;

		end = sg_dma_address(sg) + intra_off + consumed;
		if (end & (dma_size - 1)) {
			pr_warn_ratelimited("sg entry ends off a %zu-byte chunk boundary (end=%pad)\n",
					    dma_size, &end);
			return -EINVAL;
		}

		sg = sg_next(sg);
		intra_off = 0;

		if (sg && (sg_dma_address(sg) & (dma_size - 1))) {
			pr_warn_ratelimited("sg entry starts off a %zu-byte chunk boundary (dma=%pad)\n",
					    dma_size, &sg->dma_address);
			return -EINVAL;
		}
	}

	if (remaining) {
		pr_warn_ratelimited("sg mapping short by %zu bytes\n", remaining);
		return -EINVAL;
	}

	*out_cnt = total;
	return 0;
}

int mx_desc_list_init(struct mx_pci_dev *mx_pdev,
		      struct mx_transfer *transfer, size_t dma_size,
		      int descs_per_list, bool skip_first_entry,
		      size_t desc_cnt, uint64_t *out_ba)
{
	struct sg_table *sgt = &transfer->sg_ctx->sgt;
	size_t byte_offset = transfer->sg_byte_offset;
	size_t remaining = transfer->size;
	struct scatterlist *sg = NULL;
	size_t intra_off = 0;
	dma_addr_t dma_addr;
	size_t entry_avail;
	size_t len;
	uint64_t *desc;
	size_t total_desc_cnt;
	int list_cnt, list_idx, desc_idx;
	int ret;

	*out_ba = 0;

	ret = mx_sg_locate(sgt, byte_offset, &sg, &intra_off);
	if (ret) {
		pr_warn("Failed to locate sg slice (byte_offset=%zu)\n", byte_offset);
		return ret;
	}

	total_desc_cnt = desc_cnt;
	if (total_desc_cnt == 0) {
		pr_warn("desc count is 0 (byte_size=%zu, skip_first=%d)\n", remaining, skip_first_entry);
		return -EINVAL;
	}
	list_cnt = mx_get_list_count(total_desc_cnt, descs_per_list);
	ret = desc_list_alloc(mx_pdev, transfer, list_cnt);
	if (ret) {
		pr_warn("Failed to desc_list_alloc (err=%d)\n", ret);
		return ret;
	}

	list_idx = 0;
	desc_idx = 0;
	desc = (uint64_t *)transfer->desc_list_va[list_idx];

	dma_addr = sg_dma_address(sg) + intra_off;
	entry_avail = sg_dma_len(sg) - intra_off;
	len = mx_prp_first_chunk_len(sg, intra_off, dma_size);
	len = min3(len, entry_avail, remaining);

	if (skip_first_entry) {
		dma_addr += len;
		entry_avail -= len;
		remaining -= len;
		if (entry_avail == 0 && remaining > 0) {
			sg = sg_next(sg);
			if (!sg) {
				pr_warn("sg_next NULL after skip_first\n");
				desc_list_free(mx_pdev, transfer);
				return -EINVAL;
			}
			dma_addr = sg_dma_address(sg);
			entry_avail = sg_dma_len(sg);
		}
		if (dma_addr & (dma_size - 1))
			goto misaligned;
		len = min3(dma_size, entry_avail, remaining);
	}

	while (remaining > 0) {
		if (desc_idx == descs_per_list - 1 && total_desc_cnt > 1) {
			if (list_idx + 1 >= list_cnt)
				goto overrun;
			desc[desc_idx] = (uint64_t)transfer->desc_list_ba[++list_idx];
			desc = (uint64_t *)transfer->desc_list_va[list_idx];
			desc_idx = 0;
		}

		if (desc_idx >= descs_per_list || total_desc_cnt == 0)
			goto overrun;

		desc[desc_idx++] = dma_addr;
		dma_addr += len;
		entry_avail -= len;
		remaining -= len;
		total_desc_cnt--;

		if (remaining == 0)
			break;

		if (entry_avail == 0) {
			sg = sg_next(sg);
			if (!sg) {
				pr_warn("sg_next NULL mid-walk (remaining=%zu)\n", remaining);
				desc_list_free(mx_pdev, transfer);
				return -EINVAL;
			}
			dma_addr = sg_dma_address(sg);
			entry_avail = sg_dma_len(sg);
		}
		if (dma_addr & (dma_size - 1))
			goto misaligned;
		len = min3(dma_size, entry_avail, remaining);
	}

	*out_ba = transfer->desc_list_ba[0];
	return 0;

misaligned:
	pr_warn("desc walk left a %zu-byte chunk boundary (dma=%pad, remaining=%zu)\n",
		dma_size, &dma_addr, remaining);
	desc_list_free(mx_pdev, transfer);
	return -EINVAL;

overrun:
	pr_warn("desc count disagrees with emit walk (remaining=%zu, list=%d/%d, idx=%d)\n",
		remaining, list_idx, list_cnt, desc_idx);
	desc_list_free(mx_pdev, transfer);
	return -EINVAL;
}

#define BACKOFF_SPIN_ITERS	100
#define BACKOFF_BASE_SLEEP_US	8
#define BACKOFF_MAX_SLEEP_US	16000
#define BACKOFF_TICKS_PER_LEVEL	4
#define BACKOFF_MAX_SHIFT	(ilog2(BACKOFF_MAX_SLEEP_US / BACKOFF_BASE_SLEEP_US) + 1)

static inline void poll_backoff(unsigned int *idle_count)
{
	unsigned int count = min(*idle_count + 1, 255u);
	unsigned int shift, sleep_us;

	*idle_count = count;

	BUILD_BUG_ON((BACKOFF_BASE_SLEEP_US << BACKOFF_MAX_SHIFT) < BACKOFF_MAX_SLEEP_US);
	if (count <= BACKOFF_SPIN_ITERS) {
		cond_resched();
		return;
	}

	shift = min_t(unsigned int, (count - BACKOFF_SPIN_ITERS - 1) / BACKOFF_TICKS_PER_LEVEL,
		      BACKOFF_MAX_SHIFT);
	sleep_us = min_t(unsigned int, BACKOFF_BASE_SLEEP_US << shift, BACKOFF_MAX_SLEEP_US);
	usleep_range_state(sleep_us, sleep_us + (sleep_us >> 3), TASK_INTERRUPTIBLE);
}

void mx_queue_common_init(struct mx_queue *q, struct mx_pci_dev *mx_pdev, const struct mx_queue_ops *ops)
{
	q->dev = &mx_pdev->pdev->dev;
	q->mx_pdev = mx_pdev;
	q->ops = ops;
	spin_lock_init(&q->sq_lock);
	INIT_LIST_HEAD(&q->sq_list);
	init_swait_queue_head(&q->sq_wait);
	init_swait_queue_head(&q->cq_wait);
	atomic_set(&q->wait_count, 0);
	atomic_set(&q->zombie_wait_count, 0);
	atomic_set(&q->lv_health, MX_LIVENESS_ALIVE);
	q->lv_progress_jiffies = jiffies;
}

int mx_start_io_queue(struct mx_pci_dev *mx_pdev, struct mx_queue *q)
{
	int ret;

	mx_pdev->submit_thread = kthread_run(mx_submit_handler, q, "mx_submit_thd%d", mx_pdev->dev_id);
	if (IS_ERR(mx_pdev->submit_thread)) {
		ret = PTR_ERR(mx_pdev->submit_thread);
		pr_err("Failed to create submit thread (err=%d)\n", ret);
		mx_pdev->submit_thread = NULL;
		return ret;
	}
	sched_set_fifo_low(mx_pdev->submit_thread);

	mx_pdev->complete_thread = kthread_run(mx_complete_handler, q, "mx_complete_thd%d", mx_pdev->dev_id);
	if (IS_ERR(mx_pdev->complete_thread)) {
		ret = PTR_ERR(mx_pdev->complete_thread);
		pr_err("Failed to create complete thread (err=%d)\n", ret);
		kthread_stop(mx_pdev->submit_thread);
		mx_pdev->submit_thread = NULL;
		mx_pdev->complete_thread = NULL;
		return ret;
	}
	sched_set_fifo_low(mx_pdev->complete_thread);

	mx_pdev->io_queue = q;
	mx_bind_handlers_to_numa(mx_pdev);

	return 0;
}

void mx_stop_queue_threads(struct mx_pci_dev *mx_pdev)
{
	int ret;

	if (!IS_ERR_OR_NULL(mx_pdev->submit_thread)) {
		ret = kthread_stop(mx_pdev->submit_thread);
		if (ret)
			pr_err("submit_thread thread doesn't stop properly (err=%d)\n", ret);
	}
	mx_pdev->submit_thread = NULL;

	if (!IS_ERR_OR_NULL(mx_pdev->complete_thread)) {
		ret = kthread_stop(mx_pdev->complete_thread);
		if (ret)
			pr_err("complete_thread thread doesn't stop properly (err=%d)\n", ret);
	}
	mx_pdev->complete_thread = NULL;
}

static unsigned long mx_liveness_stalled_ms(struct mx_queue *q)
{
	unsigned long progress = READ_ONCE(q->lv_progress_jiffies);
	unsigned long now = jiffies;

	return time_after(now, progress) ? jiffies_to_msecs(now - progress) : 0;
}

static int mx_queue_live_count(struct mx_queue *q)
{
	return atomic_read(&q->wait_count) - atomic_read(&q->zombie_wait_count);
}

static long mx_sq_idle_timeout(struct mx_queue *q, bool lv_on)
{
	if (lv_on && (mx_queue_live_count(q) > 0 || atomic_read(&q->lv_inflight)))
		return msecs_to_jiffies(POLLING_INTERVAL_MSEC);
	return MAX_SCHEDULE_TIMEOUT;
}

static long mx_cq_idle_timeout(struct mx_queue *q)
{
	if (mx_queue_live_count(q) > 0 || atomic_read(&q->lv_inflight))
		return msecs_to_jiffies(POLLING_INTERVAL_MSEC);
	return msecs_to_jiffies(ZOMBIE_POLL_INTERVAL_MSEC);
}

static void mx_liveness_watchdog(struct mx_queue *q)
{
	struct mx_pci_dev *mx_pdev = q->mx_pdev;
	int outstanding = atomic_read(&q->wait_count) - atomic_read(&q->zombie_wait_count);
	unsigned int dead_ms = max(READ_ONCE(mx_pdev->liveness_dead_ms), 2u);
	unsigned int stall_ms = clamp(READ_ONCE(mx_pdev->liveness_stall_ms), 1u, dead_ms - 1);
	unsigned long stalled_ms;
	u64 pong_wait_ns;

	pong_wait_ns = ktime_get_ns() - READ_ONCE(q->lv_sent_ns);
	if (atomic_read(&q->lv_inflight) &&
	    pong_wait_ns > (u64)dead_ms * NSEC_PER_MSEC) {
		atomic_cmpxchg(&q->lv_health, MX_LIVENESS_SUSPECT, MX_LIVENESS_DEAD);
		if (q->ops->is_pushable(q)) {
			dev_warn_ratelimited(q->dev,
				"liveness: no pong for %llu ms (outstanding=%d), re-probing\n",
				pong_wait_ns / NSEC_PER_MSEC, outstanding);
			WRITE_ONCE(q->lv_sent_ns, ktime_get_ns());
			q->ops->build_ping_command(q->lv_ping_cmd);
			q->ops->push_command(q, q->lv_ping_cmd);
		}
		return;
	}

	if (outstanding <= 0)
		return;

	stalled_ms = mx_liveness_stalled_ms(q);

	if (stalled_ms > dead_ms && atomic_read(&q->lv_inflight) == 0 &&
	    mx_liveness_stalled_ms(q) > dead_ms)
		atomic_set(&q->lv_health, MX_LIVENESS_DEAD);

	if (stalled_ms > stall_ms && atomic_read(&q->lv_inflight) == 0 &&
	    q->ops->is_pushable(q) &&
	    atomic_cmpxchg(&q->lv_inflight, 0, 1) == 0) {
		atomic_cmpxchg(&q->lv_health, MX_LIVENESS_ALIVE, MX_LIVENESS_SUSPECT);
		WRITE_ONCE(q->lv_sent_ns, ktime_get_ns());
		q->ops->build_ping_command(q->lv_ping_cmd);
		q->ops->push_command(q, q->lv_ping_cmd);
	}
}

static unsigned int mx_sq_push_locked(struct mx_queue *q, bool lv_on, bool *went_live)
{
	const struct mx_queue_ops *ops = q->ops;
	struct mx_transfer *transfer, *tmp;
	unsigned int pushed_count = 0;
	bool no_completion;
	void *command;

	lockdep_assert_held(&q->sq_lock);
	*went_live = false;
	list_for_each_entry_safe(transfer, tmp, &q->sq_list, entry) {
		if (lv_on && atomic_read(&q->lv_inflight))
			break;
		if (!ops->is_pushable(q))
			break;

		no_completion = transfer->no_completion;
		command = transfer->command;
		list_del_init(&transfer->entry);
		trace_mx_dma_xfer_submit((u32)transfer->id, no_completion);
		if (!no_completion &&
		    atomic_inc_return(&q->wait_count) -
		    atomic_read(&q->zombie_wait_count) <= 1) {
			WRITE_ONCE(q->lv_progress_jiffies, jiffies);
			*went_live = true;
		}
		/* INTENTIONAL: a completion may free transfer once pushed; read fields first. */
		ops->push_command(q, command);
		pushed_count++;

		if (no_completion) {
			complete(&transfer->done);
		} else {
			swake_up_one(&q->cq_wait);
		}
	}
	return pushed_count;
}

void mx_queue_submit(struct mx_queue *q, struct mx_transfer **transfers, int count)
{
	unsigned int pushed_count;
	unsigned long flags;
	bool lv_on, went_live, backlog;
	int i;

	spin_lock_irqsave(&q->sq_lock, flags);
	for (i = 0; i < count; i++)
		list_add_tail(&transfers[i]->entry, &q->sq_list);
	lv_on = READ_ONCE(q->mx_pdev->liveness_enable);
	pushed_count = mx_sq_push_locked(q, lv_on, &went_live);
	if (pushed_count && q->ops->post_submit)
		q->ops->post_submit(q);
	if (pushed_count)
		trace_mx_dma_xfer_post_submit(q->mx_pdev->dev_id, pushed_count);
	backlog = !list_empty(&q->sq_list);
	spin_unlock_irqrestore(&q->sq_lock, flags);

	if (backlog || (lv_on && went_live))
		swake_up_one(&q->sq_wait);
}

int mx_submit_handler(void *arg)
{
	struct mx_queue *q = (struct mx_queue *)arg;
	const struct mx_queue_ops *ops = q->ops;
	unsigned long flags;
	unsigned int idle_count = 0;
	unsigned int pushed_count;
	bool lv_on, rearm, went_live;
	long timeout;

	while (!kthread_should_stop()) {
		lv_on = READ_ONCE(q->mx_pdev->liveness_enable);
		timeout = mx_sq_idle_timeout(q, lv_on);
		rearm = lv_on && timeout == MAX_SCHEDULE_TIMEOUT;
		__swait_event_interruptible_timeout(q->sq_wait,
				!list_empty(&q->sq_list) ||
				READ_ONCE(q->mx_pdev->liveness_enable) != lv_on ||
				(rearm && mx_queue_live_count(q) > 0) || kthread_should_stop(),
				timeout);

		lv_on = READ_ONCE(q->mx_pdev->liveness_enable);
		spin_lock_irqsave(&q->sq_lock, flags);
		pushed_count = mx_sq_push_locked(q, lv_on, &went_live);
		if (lv_on)
			mx_liveness_watchdog(q);
		if (ops->post_submit)
			ops->post_submit(q);
		if (pushed_count)
			trace_mx_dma_xfer_post_submit(q->mx_pdev->dev_id,
						      pushed_count);
		spin_unlock_irqrestore(&q->sq_lock, flags);

		if (pushed_count || list_empty(&q->sq_list))
			idle_count = 0;
		else
			poll_backoff(&idle_count);
	}

	return 0;
}

int mx_complete_handler(void *arg)
{
	struct mx_queue *q = (struct mx_queue *)arg;
	const struct mx_queue_ops *ops = q->ops;
	struct mx_transfer *transfer;
	struct mx_completion_info info;
	unsigned long id_flags;
	unsigned int idle_count = 0;

	while (!kthread_should_stop()) {
		bool popped_any = false;

		__swait_event_interruptible_timeout(q->cq_wait,
			mx_queue_live_count(q) > 0 || kthread_should_stop(),
			mx_cq_idle_timeout(q));

		while (ops->is_popable(q)) {
			popped_any = true;
			ops->pop_completion(q, &info);

			WRITE_ONCE(q->lv_progress_jiffies, jiffies);
			atomic_set(&q->lv_health, MX_LIVENESS_ALIVE);
			if (info.id == MX_PING_ID) {
				if (atomic_xchg(&q->lv_inflight, 0) == 1)
					WRITE_ONCE(q->lv_rtt_ns,
						   ktime_get_ns() - READ_ONCE(q->lv_sent_ns));
				continue;
			}
			atomic_set(&q->lv_inflight, 0);

			transfer = transfer_id_claim_completion(info.id, &id_flags);
			if (!transfer) {
				trace_mx_dma_xfer_complete_orphan((u32)info.id, info.status, info.result);
				dev_warn_ratelimited(q->dev,
					"Completion for unknown transfer (id=%d)\n", info.id);
				continue;
			}

			trace_mx_dma_xfer_complete((u32)info.id, info.status, info.result,
					READ_ONCE(transfer->is_zombie));

			atomic_dec(&q->wait_count);

			if (READ_ONCE(transfer->is_zombie)) {
				transfer_id_complete_unlock(id_flags);
				continue;
			}

			transfer->result = info.result;
			transfer->status = info.status;
			complete(&transfer->done);
			transfer_id_complete_unlock(id_flags);
		}

		if (ops->post_complete)
			ops->post_complete(q);

		if (popped_any || mx_queue_live_count(q) <= 0)
			idle_count = 0;
		else
			poll_backoff(&idle_count);
	}

	return 0;
}

static void mx_bar_map_release(struct kref *kref)
{
	struct mx_bar_map *bar_map = container_of(kref, struct mx_bar_map, kref);

	mutex_destroy(&bar_map->lock);
	kfree(bar_map);
}

struct mx_bar_map *mx_bar_map_alloc(void)
{
	struct mx_bar_map *bar_map;

	bar_map = kzalloc(sizeof(*bar_map), GFP_KERNEL);
	if (!bar_map)
		return NULL;

	kref_init(&bar_map->kref);
	mutex_init(&bar_map->lock);

	return bar_map;
}

void mx_bar_map_put(struct mx_bar_map *bar_map)
{
	kref_put(&bar_map->kref, mx_bar_map_release);
}

static void mx_bar_vma_open(struct vm_area_struct *vma)
{
	struct mx_bar_vma *bar_vma = vma->vm_private_data;
	struct mx_bar_map *bar_map = bar_vma->bar_map;

	mutex_lock(&bar_map->lock);
	kref_get(&bar_map->kref);
	bar_map->count++;
	refcount_inc(&bar_vma->refs);
	mutex_unlock(&bar_map->lock);
}

static void mx_bar_vma_close(struct vm_area_struct *vma)
{
	struct mx_bar_vma *bar_vma = vma->vm_private_data;
	struct mx_bar_map *bar_map = bar_vma->bar_map;
	struct mx_pci_dev *mx_pdev = bar_vma->mx_pdev;
	struct mx_file_ctx *owner_ctx = bar_vma->owner_ctx;
	bool last;

	mutex_lock(&bar_map->lock);
	if (!WARN_ON_ONCE(!bar_map->count)) {
		bar_map->count--;
		if (!bar_map->count)
			bar_map->mapping = NULL;
	}
	last = refcount_dec_and_test(&bar_vma->refs);
	mutex_unlock(&bar_map->lock);

	if (last) {
		mutex_lock(&mx_pdev->lease.lock);
		if (!WARN_ON_ONCE(!owner_ctx->bar_mapping_count))
			owner_ctx->bar_mapping_count--;
		mutex_unlock(&mx_pdev->lease.lock);
		kfree(bar_vma);
		mx_file_ctx_put(owner_ctx);
		mx_pdev_put(mx_pdev);
	}

	mx_bar_map_put(bar_map);
}

static const struct vm_operations_struct mx_bar_vm_ops = {
	.open	= mx_bar_vma_open,
	.close	= mx_bar_vma_close,
};

int mx_bar_mmap(struct mx_pci_dev *mx_pdev, struct mx_file_ctx *ctx,
		struct vm_area_struct *vma)
{
	struct mx_bar_map *bar_map = mx_pdev->bar_map;
	struct mx_bar_vma *bar_vma;
	resource_size_t vm_size;
	unsigned long pfn;
	uint32_t qid;
	int ret;

	bar_vma = kzalloc(sizeof(*bar_vma), GFP_KERNEL);
	if (!bar_vma)
		return -ENOMEM;

	mutex_lock(&bar_map->lock);

	if (!mx_pdev->enabled) {
		ret = -ENODEV;
		goto out_unlock;
	}

	for (qid = 0; qid < MAX_NUM_OF_MBOX; qid++) {
		if (mx_pdev->sq_mbox_list[qid]) {
			ret = -EBUSY;
			goto out_unlock;
		}
	}

	if (vma->vm_pgoff != 0) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (!(vma->vm_flags & VM_SHARED)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	vm_size = vma->vm_end - vma->vm_start;
	if (vm_size != mx_pdev->bar_mapped_size) {
		ret = -EINVAL;
		goto out_unlock;
	}

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0) || RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 6)
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= (VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
#endif

	/* INTENTIONAL: full-BAR mappers must leave driver-owned regions (v1 qid 48) alone. */
	pfn = pci_resource_start(mx_pdev->pdev, MXDMA_BAR_INDEX) >> PAGE_SHIFT;

	mutex_lock(&mx_pdev->lease.lock);
	if (ctx->bar_mapping_count == U32_MAX) {
		mutex_unlock(&mx_pdev->lease.lock);
		ret = -EOVERFLOW;
		goto out_unlock;
	}
	mutex_unlock(&mx_pdev->lease.lock);

	ret = io_remap_pfn_range(vma, vma->vm_start, pfn, vm_size,
				 vma->vm_page_prot);
	if (!ret) {
		bar_map->mapping = vma->vm_file->f_mapping;
		mx_file_ctx_get(ctx);
		bar_vma->owner_ctx = ctx;
		mutex_lock(&mx_pdev->lease.lock);
		ctx->bar_mapping_count++;
		mutex_unlock(&mx_pdev->lease.lock);
		kref_get(&mx_pdev->ref);
		bar_vma->mx_pdev = mx_pdev;
		bar_vma->bar_map = bar_map;
		refcount_set(&bar_vma->refs, 1);
		vma->vm_private_data = bar_vma;
		vma->vm_ops = &mx_bar_vm_ops;
		kref_get(&bar_map->kref);
		bar_map->count++;
		bar_vma = NULL;
	}

out_unlock:
	mutex_unlock(&bar_map->lock);
	kfree(bar_vma);
	return ret;
}

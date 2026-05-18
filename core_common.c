// SPDX-License-Identifier: <SPDX License Expression>

#include "mx_dma.h"

/******************************************************************************/
/* Descriptor list utilities                                                  */
/******************************************************************************/
int mx_get_list_count(int total_desc_cnt, int descs_per_list)
{
	int list_cnt = 1;

	while (total_desc_cnt > descs_per_list) {
		total_desc_cnt -= (descs_per_list - 1);
		list_cnt++;
	}

	return list_cnt;
}

/*
 * Locate the SG entry containing byte_offset within sgt's DMA mapping.
 * Returns 0 on hit, -EINVAL if byte_offset is past the end.  *out_intra
 * is the byte offset into the returned entry's DMA range.
 */
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

/*
 * Compute the first PRP chunk length within an SG entry, starting at
 * intra_off bytes into the entry.  The chunk is truncated to align the
 * next chunk on a dma_size boundary inside the physical page.  When the
 * starting position is already aligned, returns dma_size.
 */
size_t mx_prp_first_chunk_len(struct scatterlist *sg, size_t intra_off, size_t dma_size)
{
	size_t off_in_page = (sg->offset + intra_off) & (PAGE_SIZE - 1);
	size_t aligned;

	if (off_in_page == 0)
		return dma_size;

	aligned = (PAGE_SIZE - off_in_page) & (dma_size - 1);
	return aligned ? aligned : dma_size;
}

/*
 * Count PRP descriptors needed to cover [byte_offset, byte_offset+byte_size)
 * within sgt.  skip_first subtracts one (used when the caller stores the
 * first DMA address inline in the command's prp_entry1 slot).
 */
int mx_get_total_desc_count(struct sg_table *sgt, size_t byte_offset, size_t byte_size,
			    size_t dma_size, bool skip_first)
{
	struct scatterlist *sg = NULL;
	size_t intra_off = 0;
	size_t remaining = byte_size;
	int total = 0;
	int ret;

	if (byte_size == 0)
		return 0;

	ret = mx_sg_locate(sgt, byte_offset, &sg, &intra_off);
	if (ret)
		return 0;

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

		sg = sg_next(sg);
		intra_off = 0;
	}

	if (skip_first)
		total--;

	return total;
}

uint64_t mx_desc_list_init(struct mx_pci_dev *mx_pdev,
			   struct mx_transfer *transfer, size_t dma_size,
			   int descs_per_list, bool skip_first_entry)
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
	int total_desc_cnt, list_cnt, list_idx, desc_idx;
	int ret;

	ret = mx_sg_locate(sgt, byte_offset, &sg, &intra_off);
	if (ret) {
		pr_warn("Failed to locate sg slice (byte_offset=%zu)\n", byte_offset);
		return 0;
	}

	total_desc_cnt = mx_get_total_desc_count(sgt, byte_offset, remaining, dma_size, skip_first_entry);
	list_cnt = mx_get_list_count(total_desc_cnt, descs_per_list);
	ret = desc_list_alloc(mx_pdev, transfer, list_cnt);
	if (ret) {
		pr_warn("Failed to desc_list_alloc (err=%d)\n", ret);
		return 0;
	}

	list_idx = 0;
	desc_idx = 0;
	desc = (uint64_t *)transfer->desc_list_va[list_idx];

	dma_addr = sg_dma_address(sg) + intra_off;
	entry_avail = sg_dma_len(sg) - intra_off;
	len = mx_prp_first_chunk_len(sg, intra_off, dma_size);
	len = min3(len, entry_avail, remaining);

	if (skip_first_entry) {
		/* First slot lives in command.prp_entry{1,2}; advance past it. */
		dma_addr += len;
		entry_avail -= len;
		remaining -= len;
		if (entry_avail == 0 && remaining > 0) {
			sg = sg_next(sg);
			if (!sg) {
				pr_warn("sg_next NULL after skip_first\n");
				desc_list_free(mx_pdev, transfer);
				return 0;
			}
			dma_addr = sg_dma_address(sg);
			entry_avail = sg_dma_len(sg);
		}
		len = min3((size_t)dma_size, entry_avail, remaining);
	}

	while (remaining > 0) {
		if (desc_idx == descs_per_list - 1 && total_desc_cnt > 1) {
			desc[desc_idx] = (uint64_t)transfer->desc_list_ba[++list_idx];
			desc = (uint64_t *)transfer->desc_list_va[list_idx];
			desc_idx = 0;
		}

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
				return 0;
			}
			dma_addr = sg_dma_address(sg);
			entry_avail = sg_dma_len(sg);
		}
		len = min3((size_t)dma_size, entry_avail, remaining);
	}

	return transfer->desc_list_ba[0];
}

/******************************************************************************/
/* Adaptive backoff for poll loops                                            */
/******************************************************************************/

/*
 * When hardware is temporarily unresponsive, the handler spins with
 * cond_resched() for BACKOFF_SPIN_ITERS iterations to stay responsive,
 * then transitions to exponential sleep (125 -> 250 -> 500 -> ... -> 16000 us)
 * to reduce CPU usage while preventing soft lockup.
 */
#define BACKOFF_SPIN_ITERS	100
#define BACKOFF_BASE_SLEEP_US	125
#define BACKOFF_MAX_SLEEP_US	16000
#define BACKOFF_TICKS_PER_LEVEL	4

static inline void poll_backoff(unsigned int *idle_count)
{
	unsigned int count = min(*idle_count + 1, 255u);
	unsigned int shift, sleep_us;

	*idle_count = count;

	if (count <= BACKOFF_SPIN_ITERS) {
		cond_resched();
		return;
	}

	shift = min_t(unsigned int, (count - BACKOFF_SPIN_ITERS - 1) / BACKOFF_TICKS_PER_LEVEL, 7);
	sleep_us = min_t(unsigned int, BACKOFF_BASE_SLEEP_US << shift, BACKOFF_MAX_SLEEP_US);
	usleep_range_state(sleep_us,
			   sleep_us + max_t(unsigned int, 100, sleep_us >> 3),
			   TASK_INTERRUPTIBLE);
}

/******************************************************************************/
/* Thread helpers                                                             */
/******************************************************************************/
void mx_stop_queue_threads(struct mx_pci_dev *mx_pdev)
{
	int ret;

	if (!IS_ERR_OR_NULL(mx_pdev->submit_thread)) {
		ret = kthread_stop(mx_pdev->submit_thread);
		if (ret)
			pr_err("submit_thread thread doesn't stop properly (err=%d)\n", ret);
	}

	if (!IS_ERR_OR_NULL(mx_pdev->complete_thread)) {
		ret = kthread_stop(mx_pdev->complete_thread);
		if (ret)
			pr_err("complete_thread thread doesn't stop properly (err=%d)\n", ret);
	}
}

/******************************************************************************/
/* Unified submit/complete handlers                                           */
/******************************************************************************/
int mx_submit_handler(void *arg)
{
	struct mx_queue *q = (struct mx_queue *)arg;
	const struct mx_queue_ops *ops = q->ops;
	struct mx_transfer *transfer, *tmp;
	unsigned long flags;
	unsigned int idle_count = 0;
	bool pushed_any;

	while (!kthread_should_stop()) {
		__swait_event_interruptible_timeout(q->sq_wait,
				!list_empty(&q->sq_list),
				POLLING_INTERVAL_MSEC);

		pushed_any = false;
		spin_lock_irqsave(&q->sq_lock, flags);
		list_for_each_entry_safe(transfer, tmp, &q->sq_list, entry) {
			if (!ops->is_pushable(q))
				break;

			ops->push_command(q, transfer->command);
			list_del_init(&transfer->entry);
			pushed_any = true;

			if (transfer->no_completion) {
				/*
				 * HW guarantees no completion entry for passthru
				 * commands with no_completion set.  Signal the
				 * submitter that the command has been pushed so
				 * it can free the transfer immediately.
				 */
				complete(&transfer->done);
			} else {
				atomic_inc(&q->wait_count);
				swake_up_one(&q->cq_wait);
			}
		}
		spin_unlock_irqrestore(&q->sq_lock, flags);

		if (ops->post_submit)
			ops->post_submit(q);

		if (pushed_any)
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
	unsigned int idle_count = 0;

	while (!kthread_should_stop()) {
		bool zombie_only = (atomic_read(&q->wait_count) > 0 &&
				    atomic_read(&q->zombie_wait_count) == atomic_read(&q->wait_count));
		bool popped_any = false;

		__swait_event_interruptible_timeout(q->cq_wait,
			atomic_read(&q->wait_count) - atomic_read(&q->zombie_wait_count) > 0,
			zombie_only ? ZOMBIE_POLL_INTERVAL_MSEC : POLLING_INTERVAL_MSEC);

		while (ops->is_popable(q)) {
			popped_any = true;
			ops->pop_completion(q, &info);

			transfer = find_transfer_by_id(info.id);
			if (!transfer) {
				dev_warn_ratelimited(q->dev,
					"Completion for unknown transfer (id=%d)\n", info.id);
				continue;
			}

			/*
			 * Claim wait_count decrement — prevents double decrement
			 * if zombie_cleanup races with this completion.
			 */
			if (atomic_cmpxchg(&transfer->wait_claimed, 0, 1) != 0)
				continue;

			atomic_dec(&q->wait_count);

			if (READ_ONCE(transfer->is_zombie))
				continue;

			transfer->result = info.result;
			transfer->status = info.status;
			complete(&transfer->done);
		}

		if (ops->post_complete)
			ops->post_complete(q);

		if (popped_any)
			idle_count = 0;
		else
			poll_backoff(&idle_count);
	}

	return 0;
}

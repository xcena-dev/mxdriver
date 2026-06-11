// SPDX-License-Identifier: <SPDX License Expression>

#include "mx_dma.h"

#ifndef MX_DMA_DISABLE_TRACE
#include "trace.h"
#else
#define trace_mx_dma_xfer_submit(xfer_id, no_completion)			do { } while (0)
#define trace_mx_dma_xfer_complete(xfer_id, status, result, is_zombie)		do { } while (0)
#define trace_mx_dma_xfer_complete_orphan(xfer_id, status, result)		do { } while (0)
#endif

bool liveness_enable;
module_param(liveness_enable, bool, 0644);
MODULE_PARM_DESC(liveness_enable, "Enable device liveness ping watchdog (requires ping-capable FW)");
unsigned int liveness_stall_ms = 1000;
module_param(liveness_stall_ms, uint, 0644);
unsigned int liveness_dead_ms = 5000;
module_param(liveness_dead_ms, uint, 0644);
unsigned int liveness_max_mult = 10;
module_param(liveness_max_mult, uint, 0644);
MODULE_PARM_DESC(liveness_max_mult, "Absolute wait ceiling = timeout_ms * this (clamped to 1..1000) while transport stays alive");

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

/* Locate SG entry containing byte_offset in sgt's DMA mapping; *out_intra is the offset into the
 * found entry.  Returns 0 on hit, -EINVAL if byte_offset >= sum(sg_dma_len).  byte_offset must
 * be strictly less than the total mapped length — callers handle zero-length slices upstream. */
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

/* First PRP chunk length within an SG entry starting at intra_off; truncates so subsequent chunks
 * land on dma_size boundaries.  Returns dma_size when already aligned.  Works for arbitrary
 * dma_size (compiler folds the modulo to a bitmask when dma_size is a known power of 2). */
size_t mx_prp_first_chunk_len(struct scatterlist *sg, size_t intra_off, size_t dma_size)
{
	size_t off_in_page = (sg->offset + intra_off) & (PAGE_SIZE - 1);
	size_t rem = off_in_page % dma_size;

	return rem ? (dma_size - rem) : dma_size;
}

/* Count PRP descriptors needed for byte_size bytes starting at (sg, intra_off); caller must
 * pre-locate via mx_sg_locate.  skip_first subtracts one (when caller stashes the first DMA
 * address inline in prp_entry1).  sg/intra_off are by-value so caller's walking state survives. */
int mx_get_total_desc_count(struct scatterlist *sg, size_t intra_off,
			    size_t byte_size, size_t dma_size, bool skip_first)
{
	size_t remaining = byte_size;
	int total = 0;

	if (byte_size == 0)
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

	if (skip_first && total > 0)
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

	total_desc_cnt = mx_get_total_desc_count(sg, intra_off, remaining, dma_size, skip_first_entry);
	if (total_desc_cnt <= 0) {
		pr_warn("desc count <= 0 (byte_size=%zu, skip_first=%d)\n", remaining, skip_first_entry);
		return 0;
	}
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
/*
 * Transport liveness watchdog. Runs in the submit thread (never blocks) while
 * IO is outstanding: probes a stalled queue with a fire-and-forget ping and
 * marks the transport DEAD when neither completions nor a pong arrive in time.
 */
static void mx_liveness_watchdog(struct mx_queue *q)
{
	unsigned long now = jiffies;
	int outstanding = atomic_read(&q->wait_count) - atomic_read(&q->zombie_wait_count);
	/* Snapshot + sanitize sysfs-writable params: keep 1 <= stall < dead so a probe
	 * is always attempted before the no-completion DEAD verdict fires. */
	unsigned int dead_ms = max(liveness_dead_ms, 2u);
	unsigned int stall_ms = clamp(liveness_stall_ms, 1u, dead_ms - 1);
	unsigned long stalled_ms;

	if (outstanding <= 0)
		return;

	stalled_ms = jiffies_to_msecs(now - READ_ONCE(q->lv_progress_jiffies));

	/* No completion for too long, no probe in flight: dead (SQ-stuck case where
	 * a probe cannot even be pushed; an in-flight probe has its own pong budget
	 * below). Progress re-sampled to narrow race vs lock-free ALIVE write. */
	if (stalled_ms > dead_ms && atomic_read(&q->lv_inflight) == 0 &&
	    jiffies_to_msecs(jiffies - READ_ONCE(q->lv_progress_jiffies)) > dead_ms)
		atomic_set(&q->lv_health, MX_LIVENESS_DEAD);

	/* Probe: stalled past threshold, queue has room, no probe in flight. */
	if (stalled_ms > stall_ms && atomic_read(&q->lv_inflight) == 0 &&
	    q->ops->is_pushable(q) &&
	    atomic_cmpxchg(&q->lv_inflight, 0, 1) == 0) {
		/* Verifying: downgrade ALIVE->SUSPECT until the pong (or any
		 * completion) resolves it; leaves DEAD untouched. */
		atomic_cmpxchg(&q->lv_health, MX_LIVENESS_ALIVE, MX_LIVENESS_SUSPECT);
		WRITE_ONCE(q->lv_sent_ns, ktime_get_ns());
		q->ops->build_ping_command(q->lv_ping_cmd);
		q->ops->push_command(q, q->lv_ping_cmd);
	}

	/* Probe outstanding with no pong past the dead budget: dead. cmpxchg from
	 * SUSPECT so a pong that just resolved the window (ALIVE) is not clobbered. */
	if (atomic_read(&q->lv_inflight) &&
	    ktime_get_ns() - READ_ONCE(q->lv_sent_ns) > (u64)dead_ms * NSEC_PER_MSEC)
		atomic_cmpxchg(&q->lv_health, MX_LIVENESS_SUSPECT, MX_LIVENESS_DEAD);
}

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
			/* Ping outstanding: hold submits until the pong resolves — the liveness probe has priority. */
			if (liveness_enable && atomic_read(&q->lv_inflight))
				break;
			if (!ops->is_pushable(q))
				break;

			ops->push_command(q, transfer->command);
			list_del_init(&transfer->entry);
			pushed_any = true;

			trace_mx_dma_xfer_submit((u32)transfer->id, transfer->no_completion);

			if (transfer->no_completion) {
				/*
				 * HW guarantees no completion entry for passthru
				 * commands with no_completion set.  Signal the
				 * submitter that the command has been pushed so
				 * it can free the transfer immediately.
				 */
				complete(&transfer->done);
			} else {
				if (atomic_inc_return(&q->wait_count) == 1)
					WRITE_ONCE(q->lv_progress_jiffies, jiffies);
				swake_up_one(&q->cq_wait);
			}
		}
		if (liveness_enable)
			mx_liveness_watchdog(q);
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

			/* Unconditional by design (not gated on liveness_enable): keeps
			 * lv_progress/lv_health warm so a sysfs enable at boot won't see a
			 * stale timestamp and falsely declare DEAD on the first tick. */
			WRITE_ONCE(q->lv_progress_jiffies, jiffies);
			atomic_set(&q->lv_health, MX_LIVENESS_ALIVE);
			if (info.id == MX_PING_ID) {
				/* Record RTT only if the pong itself ended the verify window;
				 * a normal completion may have resolved it first. */
				if (atomic_xchg(&q->lv_inflight, 0) == 1)
					WRITE_ONCE(q->lv_rtt_ns,
						   ktime_get_ns() - READ_ONCE(q->lv_sent_ns));
				continue;
			}
			/* Any normal completion also ends the verify window — resume held submits. */
			atomic_set(&q->lv_inflight, 0);

			transfer = find_transfer_by_id(info.id);
			if (!transfer) {
				trace_mx_dma_xfer_complete_orphan((u32)info.id, info.status, info.result);
				dev_warn_ratelimited(q->dev,
					"Completion for unknown transfer (id=%d)\n", info.id);
				continue;
			}

			trace_mx_dma_xfer_complete((u32)info.id, info.status, info.result,
					READ_ONCE(transfer->is_zombie));

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

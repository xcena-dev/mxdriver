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

/******************************************************************************/
/* Descriptor list utilities                                                  */
/******************************************************************************/
int mx_get_list_count(size_t total_desc_cnt, int descs_per_list)
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

/* Chunk length from dma_addr to the next dma_size boundary; dma_size must be a power of 2
 * (mask instead of modulo: a 64-bit div would not link on 32-bit kernels). */
static size_t prp_chunk_len_at(dma_addr_t dma_addr, size_t dma_size)
{
	size_t rem = (size_t)(dma_addr & (dma_size - 1));

	return rem ? (dma_size - rem) : dma_size;
}

/* First PRP chunk length at (sg, intra_off): distance to the next dma_size boundary of the
 * mapped DMA address (the device splits by the address it receives; SWIOTLB may not preserve
 * the CPU page offset), clamped to the entry's remaining bytes. */
size_t mx_prp_first_chunk_len(struct scatterlist *sg, size_t intra_off, size_t dma_size)
{
	size_t len = prp_chunk_len_at(sg_dma_address(sg) + intra_off, dma_size);

	return min_t(size_t, len, sg_dma_len(sg) - intra_off);
}

/* Count PRP descriptors for byte_size bytes at (sg, intra_off) and verify the slice is
 * expressible as a PRP list; caller must pre-locate via mx_sg_locate.  sg/intra_off are by-value
 * so the caller's walking state survives.
 *
 * The device receives no per-descriptor lengths: it takes the first chunk as the distance to the
 * next dma_size boundary and every later one as a full dma_size.  Only the first descriptor may
 * therefore start mid-chunk and only the last may be short, which holds exactly when every entry
 * but the last ends on a dma_size boundary and every entry but the first starts on one.
 * dma_set_min_align_mask() keeps mappings compliant; a violation means the device would misplace
 * data, so reject it (-EINVAL) instead. */
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

		/* Checked separately from the end above: only a trailing entry may be short, so
		 * its start alignment is not implied by any entry's end. */
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

/* desc_cnt: descriptors this call will emit, i.e. mx_get_total_desc_count() less the one the
 * caller stashes inline when skip_first_entry.  Required: the caller has already walked the
 * slice, and recomputing here would only add a way for the two walks to disagree. */
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
		/* First slot lives in command.prp_entry{1,2}; advance past it. */
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

		/* total_desc_cnt is the allocation basis; emitting past it would leave the loop
		 * writing off the end of the current dma_pool page. */
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
		/* Past the first chunk the device consumes a full dma_size per descriptor, so emit
		 * that and rely on mx_get_total_desc_count() having rejected any layout where it
		 * would not fit.  Re-checked rather than re-derived: a short chunk here would be
		 * read long by the device. */
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
	mx_pdev->submit_thread = NULL;

	if (!IS_ERR_OR_NULL(mx_pdev->complete_thread)) {
		ret = kthread_stop(mx_pdev->complete_thread);
		if (ret)
			pr_err("complete_thread thread doesn't stop properly (err=%d)\n", ret);
	}
	mx_pdev->complete_thread = NULL;
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
	struct mx_pci_dev *mx_pdev = q->mx_pdev;
	unsigned long now = jiffies;
	int outstanding = atomic_read(&q->wait_count) - atomic_read(&q->zombie_wait_count);
	/* Snapshot + sanitize sysfs-writable params: keep 1 <= stall < dead so a probe
	 * is always attempted before the no-completion DEAD verdict fires. */
	unsigned int dead_ms = max(READ_ONCE(mx_pdev->liveness_dead_ms), 2u);
	unsigned int stall_ms = clamp(READ_ONCE(mx_pdev->liveness_stall_ms), 1u, dead_ms - 1);
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
	unsigned int pushed_count;
	bool lv_on;

	while (!kthread_should_stop()) {
		__swait_event_interruptible_timeout(q->sq_wait,
				!list_empty(&q->sq_list),
				POLLING_INTERVAL_MSEC);

		pushed_count = 0;
		lv_on = READ_ONCE(q->mx_pdev->liveness_enable);
		spin_lock_irqsave(&q->sq_lock, flags);
		list_for_each_entry_safe(transfer, tmp, &q->sq_list, entry) {
			/* Ping outstanding: hold submits until the pong resolves — the liveness probe has priority. */
			if (lv_on && atomic_read(&q->lv_inflight))
				break;
			if (!ops->is_pushable(q))
				break;

			ops->push_command(q, transfer->command);
			list_del_init(&transfer->entry);
			pushed_count++;

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
		if (lv_on)
			mx_liveness_watchdog(q);
		spin_unlock_irqrestore(&q->sq_lock, flags);

		if (ops->post_submit)
			ops->post_submit(q);
		if (pushed_count)
			trace_mx_dma_xfer_post_submit(q->mx_pdev->dev_id,
						      pushed_count);

		if (pushed_count)
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
			/* The waiter/cleaner serializes its final free on id_lock.
			 * Do not touch transfer after releasing completion ownership. */
			transfer_id_complete_unlock(id_flags);
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

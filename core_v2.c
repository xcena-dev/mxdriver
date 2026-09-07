
// SPDX-License-Identifier: <SPDX License Expression>

#include <linux/nvme.h>
#include <linux/sched.h>

#include "mx_dma.h"

typedef struct
{
	uint16_t depth;
	uint16_t cq_id;
	uint16_t sq_id;
	uint16_t rsvd1;
} io_queue_info_t;

struct mx_queue_v2 {
	struct mx_queue common;

	uint16_t qid;
	struct mx_command *sqes;
	struct mx_completion *cqes;
	dma_addr_t sq_dma_addr;
	dma_addr_t cq_dma_addr;

	uint32_t depth;
	uint16_t last_sq_tail;
	uint16_t last_cq_head;
	uint16_t sq_tail;
	uint16_t sq_head;
	uint16_t cq_head;
	uint16_t cq_phase;
	uint16_t admin_next_cid;
	uint16_t admin_pending_cid;
	uint8_t admin_pending_opcode;
	bool admin_pending;
	bool admin_desynced;
	bool hw_cq_created;
	bool hw_sq_created;
	uint16_t hw_cq_id;
	uint16_t hw_sq_id;
	void __iomem *db;
};


struct mx_command {
	uint8_t opcode;
	uint8_t flags;
	uint16_t command_id;
	uint32_t rsvd1;
	uint64_t rsvd2;
	uint64_t rsvd3;
	union
	{
		uint64_t host_addr;
		uint64_t prp_entry1;
		uint64_t doorbell_value;
	};
	uint64_t prp_entry2;
	union {
		uint64_t device_addr;
		io_queue_info_t io_queue_info;
	};
	uint64_t size;
	uint64_t rsvd4;
} __packed;

/*
 * Inline command storage lives in mx_transfer::cmd_inline and is sized by MX_CMD_INLINE_SIZE in mx_dma.h.
 * Enforce the budget at file scope so any future widening of struct mx_command fails the build regardless of
 * whether alloc_mx_command() is called — bumping MX_CMD_INLINE_SIZE is a deliberate, visible change.
 */
static_assert(sizeof(struct mx_command) <= MX_CMD_INLINE_SIZE,
	      "struct mx_command exceeds MX_CMD_INLINE_SIZE budget in mx_dma.h");

struct mx_completion
{
	uint64_t result;
	uint16_t sq_head;
	uint16_t sq_id;
	uint16_t command_id;
	uint16_t status;
} __packed;

/******************************************************************************/
/* Queue helpers                                                              */
/******************************************************************************/
static bool is_pushable(struct mx_queue_v2 *queue)
{
       return (queue->sq_tail + 1) % queue->depth != READ_ONCE(queue->sq_head);
}

static bool is_popable(struct mx_queue_v2 *queue)
{
	struct mx_completion *cqe;
	uint16_t status, phase;

	if (atomic_read(&queue->common.wait_count) <= 0)
		return false;

	cqe = &queue->cqes[queue->cq_head];
	status = le16_to_cpu(READ_ONCE(cqe->status));
	phase = status & 0x1;
	return phase == queue->cq_phase;
}

static void update_sq_doorbell(struct mx_queue_v2 *queue)
{
	uint32_t next_tail = queue->sq_tail + 1;

	if (next_tail == queue->depth)
		queue->sq_tail = 0;
	else
		queue->sq_tail = next_tail;
}

static void update_cq_doorbell(struct mx_queue_v2 *queue)
{
	uint32_t next_head = queue->cq_head + 1;

	if (next_head == queue->depth) {
		queue->cq_head = 0;
		queue->cq_phase ^= 1;
	} else {
		queue->cq_head = next_head;
	}
}

static void push_mx_command(struct mx_queue_v2 *queue, struct mx_command *comm)
{
	memcpy(&queue->sqes[queue->sq_tail], comm, sizeof(struct mx_command));
	dev_dbg(queue->common.dev, "SQ+ tail=0x%02x id=0x%04x op=%u ha=0x%llx;0x%llx da=0x%llx len=%llu\n",
			queue->sq_tail, comm->command_id, comm->opcode, comm->host_addr, comm->prp_entry2, comm->device_addr, comm->size);
	update_sq_doorbell(queue);
}

static void pop_mx_completion(struct mx_queue_v2 *queue, struct mx_completion *cmpl)
{
	memcpy(cmpl, &queue->cqes[queue->cq_head], sizeof(struct mx_completion));
	dev_dbg(queue->common.dev, "CQ- head=0x%02x id=0x%04x res=0x%llx\n",
			queue->cq_head, cmpl->command_id, cmpl->result);
	WRITE_ONCE(queue->sq_head, cmpl->sq_head);
	update_cq_doorbell(queue);
}

static void ring_sq_doorbell(struct mx_queue_v2 *queue)
{
	if (queue->last_sq_tail == queue->sq_tail)
		return;

	writel(queue->sq_tail, queue->db);
	queue->last_sq_tail = queue->sq_tail;
}

static void ring_cq_doorbell(struct mx_queue_v2 *queue)
{
	if (queue->last_cq_head == queue->cq_head)
		return;

	writel(queue->cq_head, queue->db + sizeof(uint32_t));
	queue->last_cq_head = queue->cq_head;
}

/******************************************************************************/
/* Queue ops adapter for unified handlers                                     */
/******************************************************************************/
static bool v2_is_pushable(struct mx_queue *q)
{
	struct mx_queue_v2 *queue = container_of(q, struct mx_queue_v2, common);

	return is_pushable(queue);
}

static void v2_push_command(struct mx_queue *q, void *command)
{
	struct mx_queue_v2 *queue = container_of(q, struct mx_queue_v2, common);

	push_mx_command(queue, (struct mx_command *)command);
}

static void v2_post_submit(struct mx_queue *q)
{
	struct mx_queue_v2 *queue = container_of(q, struct mx_queue_v2, common);

	ring_sq_doorbell(queue);
}

static bool v2_is_popable(struct mx_queue *q)
{
	struct mx_queue_v2 *queue = container_of(q, struct mx_queue_v2, common);

	return is_popable(queue);
}

static void v2_pop_completion(struct mx_queue *q, struct mx_completion_info *info)
{
	struct mx_queue_v2 *queue = container_of(q, struct mx_queue_v2, common);
	struct mx_completion cmpl;

	pop_mx_completion(queue, &cmpl);
	info->id = cmpl.command_id;
	info->result = cmpl.result;
	info->status = 0;
}

static void v2_post_complete(struct mx_queue *q)
{
	struct mx_queue_v2 *queue = container_of(q, struct mx_queue_v2, common);

	ring_cq_doorbell(queue);
}

static void build_ping_command_v2(void *cmd)
{
	struct mx_command *comm = (struct mx_command *)cmd;

	memset(comm, 0, sizeof(*comm));
	comm->opcode = IO_OPCODE_PING;
	comm->command_id = MX_PING_ID;
}

static const struct mx_queue_ops v2_queue_ops = {
	.is_pushable	= v2_is_pushable,
	.push_command	= v2_push_command,
	.post_submit	= v2_post_submit,
	.is_popable	= v2_is_popable,
	.pop_completion	= v2_pop_completion,
	.post_complete	= v2_post_complete,
	.build_ping_command = build_ping_command_v2,
};

/******************************************************************************/
/* Transfer                                                                   */
/******************************************************************************/
#define SINGLE_DMA_SIZE		PAGE_SIZE
#define NUM_OF_DESC_PER_LIST	(SINGLE_DMA_SIZE / sizeof(uint64_t))

/* transfer.c slices parallel transfers on host-page boundaries; they must land on chunk
 * boundaries. */
static_assert((PAGE_SIZE % SINGLE_DMA_SIZE) == 0,
	      "v2 PRP chunking requires SINGLE_DMA_SIZE to divide PAGE_SIZE");

static struct mx_command *alloc_mx_command(struct mx_transfer *transfer, int opcode)
{
	struct mx_command *comm = (struct mx_command *)transfer->cmd_inline;

	memset(comm, 0, sizeof(*comm));

	comm->opcode = opcode;
	comm->command_id = transfer->id;
	comm->size = transfer->size;
	comm->device_addr = transfer->device_addr;

	return comm;
}

static void *create_mx_command_sg(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer, int opcode)
{
	struct mx_command *comm;
	struct sg_table *sgt = &transfer->sg_ctx->sgt;
	struct scatterlist *sg = NULL;
	size_t intra_off = 0;
	size_t desc_cnt;
	int ret;

	comm = alloc_mx_command(transfer, opcode);
	if (!comm) {
		pr_warn("Failed to allocate mx_command for sg transfer\n");
		return ERR_PTR(-ENOMEM);
	}

	ret = mx_sg_locate(sgt, transfer->sg_byte_offset, &sg, &intra_off);
	if (ret) {
		pr_warn("Failed to locate sg slice (id=%u)\n", transfer->id);
		return ERR_PTR(ret);
	}

	comm->prp_entry1 = sg_dma_address(sg) + intra_off;
	if (!comm->prp_entry1) {
		pr_warn("Failed to get sg_dma_address\n");
		return ERR_PTR(-EINVAL);
	}

	/* Branch on the DMA-side entry count, not host page count (alignments can differ).
	 * This also validates that the slice is expressible as a PRP list. */
	ret = mx_get_total_desc_count(sg, intra_off, transfer->size, SINGLE_DMA_SIZE, &desc_cnt);
	if (ret || desc_cnt == 0) {
		pr_warn("Failed to count descs (err=%d, cnt=%zu, id=%u)\n", ret, desc_cnt, transfer->id);
		return ERR_PTR(ret ? ret : -EINVAL);
	}

	if (desc_cnt == 1) {
		comm->prp_entry2 = 0;
	} else if (desc_cnt == 2) {
		size_t first_len = mx_prp_first_chunk_len(sg, intra_off, SINGLE_DMA_SIZE);

		/* Second PRP entry points to the chunk after the first. */
		if (intra_off + first_len < sg_dma_len(sg)) {
			comm->prp_entry2 = comm->prp_entry1 + first_len;
		} else {
			struct scatterlist *next = sg_next(sg);

			if (!next) {
				pr_warn("sg_next NULL in 2-entry path (id=%u)\n", transfer->id);
				return ERR_PTR(-EINVAL);
			}
			comm->prp_entry2 = sg_dma_address(next);
		}

		if (!comm->prp_entry2) {
			pr_warn("Failed to get sg_dma_address\n");
			return ERR_PTR(-EINVAL);
		}
	} else {
		ret = mx_desc_list_init(mx_pdev, transfer, SINGLE_DMA_SIZE, NUM_OF_DESC_PER_LIST,
					true, desc_cnt - 1, &comm->prp_entry2);
		if (ret) {
			pr_warn("Failed to desc_list_init (err=%d)\n", ret);
			return ERR_PTR(ret);
		}
	}

	return (void*)comm;
}

static void *create_mx_command_ctrl(struct mx_transfer *transfer, int opcode)
{
	struct mx_command *comm;

	comm = alloc_mx_command(transfer, opcode);
	if (!comm) {
		pr_warn("Failed to allocate mx_command\n");
		return NULL;
	}

	if (transfer->dir != DMA_TO_DEVICE)
		return (void*)comm;

	if (access_ok(transfer->user_addr, transfer->size)) {
		if (copy_from_user(&comm->doorbell_value, transfer->user_addr, sizeof(uint64_t))) {
			pr_warn("Failed to copy_from_user (%llx <- %llx)\n",
					(uint64_t)&comm->doorbell_value, (uint64_t)transfer->user_addr);
			return NULL;
		}
	} else {
		comm->doorbell_value = *(uint64_t *)transfer->user_addr;
	}

	return (void*)comm;
}

/******************************************************************************/
/* Init                                                                       */
/******************************************************************************/
static int alloc_queue(struct device *dev, struct mx_queue_v2 *queue, uint32_t q_depth)
{
	queue->depth = q_depth;
	queue->cqes = dma_alloc_coherent(dev,
			queue->depth * sizeof(struct mx_completion),
			&queue->cq_dma_addr, GFP_KERNEL);
	if (!queue->cqes)
		return -ENOMEM;

	queue->sqes = dma_alloc_coherent(dev,
			queue->depth * sizeof(struct mx_command),
			&queue->sq_dma_addr, GFP_KERNEL);
	if (!queue->sqes) {
		dma_free_coherent(dev,
				  queue->depth * sizeof(struct mx_completion),
				  queue->cqes, queue->cq_dma_addr);
		queue->cqes = NULL;
		return -ENOMEM;
	}

	pr_info("Allocated queue (depth=%u, sq_dma_addr=0x%llx, cq_dma_addr=0x%llx, sqes=0x%llx, cqes=0x%llx)\n",
			queue->depth, queue->sq_dma_addr, queue->cq_dma_addr, (uint64_t)queue->sqes, (uint64_t)queue->cqes);

	return 0;
}

static void free_queue_storage(struct device *dev, struct mx_queue_v2 *queue)
{
	if (!queue)
		return;
	if (queue->sqes) {
		dma_free_coherent(dev,
				  queue->depth * sizeof(struct mx_command),
				  queue->sqes, queue->sq_dma_addr);
		queue->sqes = NULL;
	}
	if (queue->cqes) {
		dma_free_coherent(dev,
				  queue->depth * sizeof(struct mx_completion),
				  queue->cqes, queue->cq_dma_addr);
		queue->cqes = NULL;
	}
}

static void configure_queue(struct mx_pci_dev *mx_pdev, struct mx_queue_v2 *queue, uint16_t qid)
{
	uint64_t __iomem *dbs = mx_pdev->bar + NVME_REG_DBS;

	queue->common.dev = &mx_pdev->pdev->dev;
	queue->common.mx_pdev = mx_pdev;
	queue->qid = qid;
	queue->sq_tail = 0;
	queue->sq_head = 0;
	queue->cq_head = 0;
	queue->cq_phase = 1;
	queue->db = &dbs[qid];
	memset((void *)queue->cqes, 0, queue->depth * sizeof(struct mx_completion));
	memset((void *)queue->sqes, 0, queue->depth * sizeof(struct mx_command));
	wmb();
}

static int configure_admin_queue(struct mx_pci_dev *mx_pdev)
{
	struct device *dev = &mx_pdev->pdev->dev;
	struct mx_queue_v2 *queue = kzalloc(sizeof(*queue), GFP_KERNEL);
	uint32_t aqa;
	int ret;

	pr_info("Configuring admin queue...\n");

	if (!queue)
		return -ENOMEM;

	ret = alloc_queue(dev, queue, NVME_AQ_DEPTH);
	if (ret) {
		kfree(queue);
		return ret;
	}

	queue->common.dev = dev;
	queue->common.mx_pdev = mx_pdev;
	atomic_set(&queue->common.wait_count, 0);
	mx_pdev->admin_queue = (struct mx_queue *)queue;

	aqa = queue->depth - 1;
	aqa |= aqa << 16;
	/* From this write onward the device knows host DMA addresses. Any
	 * ambiguous admin completion must retain this backing until reboot/reset. */
	mx_pdev->queue_dma_programmed = true;
	writel(aqa, mx_pdev->bar + 0x0);
	writeq(queue->sq_dma_addr, mx_pdev->bar + 0x8);
	writeq(queue->cq_dma_addr, mx_pdev->bar + 0x10);

	pr_info("Admin queue created (depth=%u)\n", queue->depth);

	configure_queue(mx_pdev, queue, 0);

	return 0;
}

static void record_admin_terminal(struct mx_queue_v2 *admin_queue,
				  u8 opcode, u16 status, u64 result)
{
	struct mx_queue_v2 *io_queue =
		(struct mx_queue_v2 *)admin_queue->common.mx_pdev->io_queue;

	if (status || !io_queue)
		return;
	switch (opcode) {
	case ADMIN_OPCODE_CREATE_IO_CQ:
		io_queue->hw_cq_created = true;
		io_queue->hw_cq_id = (u16)result;
		break;
	case ADMIN_OPCODE_CREATE_IO_SQ:
		io_queue->hw_sq_created = true;
		io_queue->hw_sq_id = (u16)result;
		break;
	case ADMIN_OPCODE_DELETE_IO_SQ:
		io_queue->hw_sq_created = false;
		break;
	case ADMIN_OPCODE_DELETE_IO_CQ:
		io_queue->hw_cq_created = false;
		break;
	default:
		break;
	}
}

static bool submit_sync_command(struct mx_queue_v2* queue, struct mx_command *c, uint64_t *result)
{
	struct mx_completion cmpl;
	struct mx_pci_dev *mx_pdev = queue->common.mx_pdev;
	u16 cid;
	u16 status;
	int timeout = 500;
	int count = 0;

	if (READ_ONCE(queue->admin_desynced))
		return false;
	cid = ++queue->admin_next_cid;
	if (!cid)
		cid = ++queue->admin_next_cid;
	c->command_id = cpu_to_le16(cid);

	for (count = 0; count < timeout; count++) {
		if (is_pushable(queue))
			break;
		msleep(1);
	}
	if (count >= timeout) {
		pr_err("Timeout waiting for pushable admin queue\n");
		/* This command has not been copied to the SQ or doorbelled, so
		 * there is no ambiguous hardware operation to drain.  The caller
		 * may retry or unwind using the last terminal queue state. */
		return false;
	}

	push_mx_command(queue, c);
	ring_sq_doorbell(queue);
	atomic_inc(&queue->common.wait_count);
	queue->admin_pending = true;
	queue->admin_pending_cid = cid;
	queue->admin_pending_opcode = c->opcode;

	for (count = 0; count < timeout; count++) {
		if (is_popable(queue)) {
			pop_mx_completion(queue, &cmpl);
			ring_cq_doorbell(queue);
			if (le16_to_cpu(cmpl.command_id) != cid) {
				pr_warn("Ignoring stale admin completion (expected cid=%u, got=%u)\n",
					cid, le16_to_cpu(cmpl.command_id));
				continue;
			}
			goto terminal;
		}
		msleep(1);
	}
	pr_err("Timeout waiting for admin completion (cid=%u op=%u)\n",
	       cid, c->opcode);
	WRITE_ONCE(queue->admin_desynced, true);
	WRITE_ONCE(mx_pdev->protocol_poisoned, true);
	return false;

terminal:
	atomic_dec(&queue->common.wait_count);
	queue->admin_pending = false;
	status = le16_to_cpu(cmpl.status) >> 1;
	record_admin_terminal(queue, c->opcode, status,
			      le64_to_cpu(cmpl.result));
	if (status) {
		pr_err("Admin command cid=%u failed with status=%#x\n", cid, status);
		return false;
	}

	if (result)
		*result = le64_to_cpu(cmpl.result);

	return true;
}

static bool delete_io_sq(struct mx_queue_v2 *admin_queue, uint16_t qid)
{
	struct mx_command comm = {};

	comm.opcode = ADMIN_OPCODE_DELETE_IO_SQ;
	comm.io_queue_info.sq_id = qid;
	return submit_sync_command(admin_queue, &comm, NULL);
}

static bool delete_io_cq(struct mx_queue_v2 *admin_queue, uint16_t qid)
{
	struct mx_command comm = {};

	comm.opcode = ADMIN_OPCODE_DELETE_IO_CQ;
	comm.io_queue_info.cq_id = qid;
	return submit_sync_command(admin_queue, &comm, NULL);
}

static int drain_pending_admin(struct mx_pci_dev *mx_pdev)
{
	struct mx_queue_v2 *queue = (struct mx_queue_v2 *)mx_pdev->admin_queue;
	struct mx_completion cmpl;
	u16 status;

	if (!queue)
		return 0;
	if (queue->admin_desynced && !queue->admin_pending)
		return -EUCLEAN;

	while (queue->admin_pending) {
		if (pci_dev_is_disconnected(mx_pdev->pdev))
			return -ENODEV;
		if (!is_popable(queue)) {
			msleep(1);
			continue;
		}
		pop_mx_completion(queue, &cmpl);
		ring_cq_doorbell(queue);
		if (le16_to_cpu(cmpl.command_id) != queue->admin_pending_cid) {
			pr_warn_ratelimited("Ignoring stale admin completion while draining (expected cid=%u, got=%u)\n",
				queue->admin_pending_cid,
				le16_to_cpu(cmpl.command_id));
			continue;
		}
		atomic_dec(&queue->common.wait_count);
		queue->admin_pending = false;
		status = le16_to_cpu(cmpl.status) >> 1;
		record_admin_terminal(queue, queue->admin_pending_opcode, status,
				      le64_to_cpu(cmpl.result));
		if (status)
			pr_err("Late admin command cid=%u terminated with status=%#x\n",
			       queue->admin_pending_cid, status);
	}
	queue->admin_desynced = false;
	return 0;
}

static void disable_admin_queue(struct mx_pci_dev *mx_pdev)
{
	/* No admin command is pending when called. Clear device-visible host
	 * addresses before coherent backing can be freed or Bus Master re-enabled. */
	writel(0, mx_pdev->bar + 0x0);
	writeq(0, mx_pdev->bar + 0x8);
	writeq(0, mx_pdev->bar + 0x10);
	wmb();
	mx_pdev->queue_dma_programmed = false;
}

static int recover_mx_queue(struct mx_pci_dev *mx_pdev)
{
	struct mx_queue_v2 *admin_queue =
		(struct mx_queue_v2 *)mx_pdev->admin_queue;
	struct mx_queue_v2 *io_queue = (struct mx_queue_v2 *)mx_pdev->io_queue;
	int ret;

	if (!admin_queue)
		return 0;
	ret = drain_pending_admin(mx_pdev);
	if (ret)
		return ret;
	WRITE_ONCE(mx_pdev->protocol_poisoned, false);
	if (!io_queue) {
		disable_admin_queue(mx_pdev);
		return 0;
	}

	/* A late CREATE may have succeeded. Tear down whatever terminal state was
	 * recorded, always SQ before its referenced CQ. A fresh timeout is retained
	 * in admin_pending and the caller retries this recovery without freeing. */
	if (io_queue->hw_sq_created &&
	    !delete_io_sq(admin_queue, io_queue->hw_sq_id)) {
		WRITE_ONCE(mx_pdev->protocol_poisoned, true);
		return -EIO;
	}
	if (io_queue->hw_cq_created &&
	    !delete_io_cq(admin_queue, io_queue->hw_cq_id)) {
		WRITE_ONCE(mx_pdev->protocol_poisoned, true);
		return -EIO;
	}
	disable_admin_queue(mx_pdev);
	return 0;
}

static int configure_io_queue(struct mx_pci_dev *mx_pdev)
{
	struct device *dev = &mx_pdev->pdev->dev;
	struct mx_queue_v2 *admin_queue = (struct mx_queue_v2 *)mx_pdev->admin_queue;
	struct mx_queue_v2 *io_queue = kzalloc(sizeof(*io_queue), GFP_KERNEL);
	struct mx_command comm = {};
	uint64_t result;
	uint16_t cq_id, sq_id;
	int ret;

	pr_info("Configuring IO queue...\n");

	if (!io_queue)
		return -ENOMEM;

	ret = alloc_queue(dev, io_queue, 256);
	if (ret) {
		kfree(io_queue);
		return ret;
	}
	io_queue->common.dev = dev;
	io_queue->common.mx_pdev = mx_pdev;
	spin_lock_init(&io_queue->common.sq_lock);
	INIT_LIST_HEAD(&io_queue->common.sq_list);
	init_swait_queue_head(&io_queue->common.sq_wait);
	init_swait_queue_head(&io_queue->common.cq_wait);
	atomic_set(&io_queue->common.wait_count, 0);
	atomic_set(&io_queue->common.zombie_wait_count, 0);
	mx_pdev->io_queue = (struct mx_queue *)io_queue;

	comm.opcode = ADMIN_OPCODE_CREATE_IO_CQ;
	comm.host_addr = cpu_to_le64(io_queue->cq_dma_addr);
	comm.io_queue_info.depth = io_queue->depth;
	if (!submit_sync_command(admin_queue, &comm, &result)) {
		pr_err("Failed to create IO completion queue\n");
		return -EIO;
	}
	cq_id = (u16)result;

	comm.opcode = ADMIN_OPCODE_CREATE_IO_SQ;
	comm.host_addr = cpu_to_le64(io_queue->sq_dma_addr);
	comm.io_queue_info.cq_id = cq_id;
	if (!submit_sync_command(admin_queue, &comm, &result)) {
		pr_err("Failed to create IO submission queue\n");
		/* A timeout/CID mismatch desynchronizes the admin queue, so no further
		 * command is safe. A matching error completion is terminal; delete the
		 * already-created CQ below. */
		if (READ_ONCE(admin_queue->admin_desynced))
			return -EIO;
		ret = -EIO;
		goto out_delete;
	}
	sq_id = (u16)result;

	if (cq_id != sq_id) {
		pr_err("Failed to create IO queue (cq_id=%d, sq_id=%d)\n", cq_id, sq_id);
		ret = -EINVAL;
		goto out_delete;
	}

	/* cq_id is device-supplied; its doorbell lives at bar + NVME_REG_DBS +
	 * cq_id * sizeof(u64). Reject an id whose doorbell would fall outside the
	 * mapped BAR before configure_queue() turns it into an MMIO pointer. */
	if (NVME_REG_DBS + ((size_t)cq_id + 1) * sizeof(uint64_t) >
	    mx_pdev->bar_mapped_size) {
		pr_err("IO queue id %u doorbell exceeds mapped BAR (%llu bytes)\n",
		       cq_id, (unsigned long long)mx_pdev->bar_mapped_size);
		ret = -EIO;
		goto out_delete;
	}

	pr_info("IO queue created (depth=%u, sq_id=%u, cq_id=%u)\n", io_queue->depth, sq_id, cq_id);

	configure_queue(mx_pdev, io_queue, cq_id);

	io_queue->common.ops = &v2_queue_ops;
	atomic_set(&io_queue->common.lv_health, MX_LIVENESS_ALIVE);
	io_queue->common.lv_progress_jiffies = jiffies;

	mx_pdev->submit_thread = kthread_run(mx_submit_handler, &io_queue->common, "mx_submit_thd%d", mx_pdev->dev_id);
	if (IS_ERR(mx_pdev->submit_thread)) {
		ret = PTR_ERR(mx_pdev->submit_thread);
		pr_err("Failed to create submit thread (err=%d)\n", ret);
		mx_pdev->submit_thread = NULL;
		goto out_delete;
	}
	/* See core_v1.c: SCHED_FIFO (lowest RT band) for low scheduling latency. */
	sched_set_fifo_low(mx_pdev->submit_thread);

	mx_pdev->complete_thread = kthread_run(mx_complete_handler, &io_queue->common, "mx_complete_thd%d", mx_pdev->dev_id);
	if (IS_ERR(mx_pdev->complete_thread)) {
		ret = PTR_ERR(mx_pdev->complete_thread);
		pr_err("Failed to create complete thread (err=%d)\n", ret);
		mx_pdev->complete_thread = NULL;
		kthread_stop(mx_pdev->submit_thread);
		mx_pdev->submit_thread = NULL;
		goto out_delete;
	}
	sched_set_fifo_low(mx_pdev->complete_thread);

	mx_bind_handlers_to_numa(mx_pdev);

	return 0;

out_delete:
	/* SQ references CQ, so unwind in dependency order. Ambiguous commands are
	 * retained and drained by recover_mx_queue() during the real unbind. */
	if (io_queue->hw_sq_created &&
	    !delete_io_sq(admin_queue, io_queue->hw_sq_id)) {
		pr_err("Failed to unwind IO submission queue\n");
		WRITE_ONCE(mx_pdev->protocol_poisoned, true);
		return ret;
	}
	if (io_queue->hw_cq_created &&
	    !delete_io_cq(admin_queue, io_queue->hw_cq_id)) {
		pr_err("Failed to unwind IO completion queue\n");
		WRITE_ONCE(mx_pdev->protocol_poisoned, true);
	}
	return ret;
}

static int release_io_queue(struct mx_pci_dev *mx_pdev)
{
	struct mx_queue_v2 *admin_queue = (struct mx_queue_v2 *)mx_pdev->admin_queue;
	struct mx_queue_v2 *io_queue = (struct mx_queue_v2 *)mx_pdev->io_queue;
	bool ok = true;

	if (!admin_queue || !io_queue)
		return 0;

	/* No live waiter remains once the per-device workqueue is drained. Stop
	 * software queue access first, then tear down SQ before its referenced CQ. */
	mx_stop_queue_threads(mx_pdev);
	if (io_queue->hw_sq_created &&
	    !delete_io_sq(admin_queue, io_queue->hw_sq_id)) {
		pr_err("Failed to delete IO submission queue\n");
		ok = false;
	}
	if (ok && io_queue->hw_cq_created &&
	    !delete_io_cq(admin_queue, io_queue->hw_cq_id)) {
		pr_err("Failed to delete IO completion queue\n");
		ok = false;
	}
	if (ok)
		disable_admin_queue(mx_pdev);

	return ok ? 0 : -EIO;
}

static int init_mx_queue(struct mx_pci_dev *mx_pdev)
{
	int ret;

	mx_pdev->page_size = SINGLE_DMA_SIZE;

	ret = configure_admin_queue(mx_pdev);
	if (ret) {
		pr_err("Failed to configure admin queue (err=%d)\n", ret);
		return ret;
	}

	ret = configure_io_queue(mx_pdev);
	if (ret) {
		pr_err("Failed to configure IO queue (err=%d)\n", ret);
		return ret;
	}

	pr_info("MX queue initialized successfully\n");
	return 0;
}

static int release_mx_queue(struct mx_pci_dev *mx_pdev)
{
	int ret;

	ret = release_io_queue(mx_pdev);
	if (ret) {
		pr_err("Failed to release IO queue (err=%d)\n", ret);
		return ret;
	}

	pr_info("MX queue released successfully\n");
	return 0;
}

static void free_mx_queue(struct mx_pci_dev *mx_pdev)
{
	struct device *dev = &mx_pdev->pdev->dev;
	struct mx_queue_v2 *io_queue = (struct mx_queue_v2 *)mx_pdev->io_queue;
	struct mx_queue_v2 *admin_queue = (struct mx_queue_v2 *)mx_pdev->admin_queue;

	if (io_queue) {
		free_queue_storage(dev, io_queue);
		kfree(io_queue);
		mx_pdev->io_queue = NULL;
	}
	if (admin_queue) {
		free_queue_storage(dev, admin_queue);
		kfree(admin_queue);
		mx_pdev->admin_queue = NULL;
	}
}

void register_mx_ops_v2(struct mx_operations *ops)
{
	ops->init_queue =  init_mx_queue;
	ops->release_queue = release_mx_queue;
	ops->recover_queue = recover_mx_queue;
	ops->free_queue = free_mx_queue;
	ops->create_command_sg = create_mx_command_sg;
	ops->create_command_ctrl = create_mx_command_ctrl;
}

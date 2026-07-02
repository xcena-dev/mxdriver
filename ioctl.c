// SPDX-License-Identifier: <SPDX License Expression>

#include "mx_dma.h"

#ifndef MX_DMA_DISABLE_TRACE
#include "trace.h"
#else
#define trace_mx_dma_ioctl_enter(dev_id, cmd)				do { } while (0)
#define trace_mx_dma_ioctl_exit(dev_id, cmd, ret)			do { } while (0)
#define trace_mx_dma_ctrl_read_enter(dev_id, qid, op, sz, da, nw)	do { } while (0)
#define trace_mx_dma_ctrl_read_exit(dev_id, qid, op, ret)		do { } while (0)
#define trace_mx_dma_ctrl_write_enter(dev_id, qid, op, sz, da, nw)	do { } while (0)
#define trace_mx_dma_ctrl_write_exit(dev_id, qid, op, ret)		do { } while (0)
#define trace_mx_dma_data_read_enter(dev_id, qid, op, sz, da, nw)	do { } while (0)
#define trace_mx_dma_data_read_exit(dev_id, qid, op, ret)		do { } while (0)
#define trace_mx_dma_data_write_enter(dev_id, qid, op, sz, da, nw)	do { } while (0)
#define trace_mx_dma_data_write_exit(dev_id, qid, op, ret)		do { } while (0)
#endif

/* Traced wrappers around the four ctrl/data primitives. They are inlined and,
 * when tracing is compiled out, collapse to a direct call. */
static __always_inline ssize_t traced_read_ctrl(struct mx_pci_dev *mx_pdev, uint32_t qid,
		char __user *buf, size_t size, loff_t *fpos, int opcode)
{
	ssize_t ret;

	trace_mx_dma_ctrl_read_enter(mx_pdev->dev_id, qid, opcode, size, (u64)*fpos, false);
	ret = read_ctrl_from_device(mx_pdev, buf, size, fpos, opcode);
	trace_mx_dma_ctrl_read_exit(mx_pdev->dev_id, qid, opcode, ret);
	return ret;
}

static __always_inline ssize_t traced_write_ctrl(struct mx_pci_dev *mx_pdev, uint32_t qid,
		const char __user *buf, size_t size, loff_t *fpos, int opcode, bool nowait)
{
	ssize_t ret;

	trace_mx_dma_ctrl_write_enter(mx_pdev->dev_id, qid, opcode, size, (u64)*fpos, nowait);
	ret = write_ctrl_to_device(mx_pdev, buf, size, fpos, opcode, nowait);
	trace_mx_dma_ctrl_write_exit(mx_pdev->dev_id, qid, opcode, ret);
	return ret;
}

static __always_inline ssize_t traced_read_data(struct mx_pci_dev *mx_pdev, uint32_t qid,
		char __user *buf, size_t size, loff_t *fpos, int opcode)
{
	ssize_t ret;

	trace_mx_dma_data_read_enter(mx_pdev->dev_id, qid, opcode, size, (u64)*fpos, false);
	ret = read_data_from_device(mx_pdev, buf, size, fpos, opcode);
	trace_mx_dma_data_read_exit(mx_pdev->dev_id, qid, opcode, ret);
	return ret;
}

static __always_inline ssize_t traced_write_data(struct mx_pci_dev *mx_pdev, uint32_t qid,
		const char __user *buf, size_t size, loff_t *fpos, int opcode, bool nowait)
{
	ssize_t ret;

	trace_mx_dma_data_write_enter(mx_pdev->dev_id, qid, opcode, size, (u64)*fpos, nowait);
	ret = write_data_to_device(mx_pdev, buf, size, fpos, opcode, nowait);
	trace_mx_dma_data_write_exit(mx_pdev->dev_id, qid, opcode, ret);
	return ret;
}

/* Note: MX_IOCTL_READ_DATA / MX_IOCTL_WRITE_DATA are intentionally NOT
 * wrapped with traced_* primitives. These ioctls operate on raw user_addr /
 * device_addr (no mailbox / qid context), and dispatch via the *_parallel
 * helpers which internally spawn multiple sub-transfers. The outer
 * ioctl_enter/exit slice captured in ioctl_to_device() covers the full
 * operation; per-sub-transfer slices would dramatically inflate ring
 * buffer volume without adding a meaningful per-op breakdown. */

struct mx_ioctl_mbox_info
{
	uint32_t qid;
	uint64_t sq_ctx_addr;
	uint64_t sq_data_addr;
	uint64_t cq_ctx_addr;
	uint64_t cq_data_addr;
};

struct mx_ioctl_cmd_with_data
{
	uint32_t qid;
	uint64_t *cmd;
	void *user_addr;
	uint64_t device_addr;
	size_t size;
};

struct mx_ioctl_cmds
{
	uint32_t qid;
	uint32_t nr_cmds;
	uint64_t *cmds;
};

struct mx_ioctl_data
{
	void *user_addr;
	uint64_t device_addr;
	size_t size;
	bool no_wait;
};

struct mx_ioctl_passthru_cmd
{
	uint64_t device_addr;
	uint64_t host_addr; /* output */
	uint64_t size;
	uint16_t subopcode;
	uint8_t  no_completion;
	uint8_t  status;    /* output */
};

struct mx_ioctl_protocol_cmd
{
	char __user *buf;
	size_t size;
};

#define MX_IOCTL_MAGIC			'X'
#define MX_IOCTL_REGISTER_MBOX		_IOW(MX_IOCTL_MAGIC, 1, struct mx_ioctl_mbox_info)
#define MX_IOCTL_INIT_MBOX		_IOW(MX_IOCTL_MAGIC, 2, uint32_t)
#define MX_IOCTL_SEND_CMD_WITH_DATA	_IOW(MX_IOCTL_MAGIC, 3, struct mx_ioctl_cmd_with_data)
#define MX_IOCTL_RECV_CMDS		_IOWR(MX_IOCTL_MAGIC, 4, struct mx_ioctl_cmds)
#define MX_IOCTL_SEND_CMDS		_IOWR(MX_IOCTL_MAGIC, 5, struct mx_ioctl_cmds)
#define MX_IOCTL_READ_DATA		_IOW(MX_IOCTL_MAGIC, 6, struct mx_ioctl_data)
#define MX_IOCTL_WRITE_DATA		_IOW(MX_IOCTL_MAGIC, 7, struct mx_ioctl_data)
#define MX_IOCTL_PASSTHRU_CMD		_IOWR(MX_IOCTL_MAGIC, 8, struct mx_ioctl_passthru_cmd)
#define MX_IOCTL_HIO_SEND		_IOW(MX_IOCTL_MAGIC, 9, struct mx_ioctl_protocol_cmd)
#define MX_IOCTL_HIO_RECV		_IOW(MX_IOCTL_MAGIC, 10, struct mx_ioctl_protocol_cmd)

/* MX_IOCTL_* indices must match mx_dma_ioctl_nr_names in trace.h AND both switches in
 * ioctl_to_device (setup-only early-return + main dispatch).  Boundary asserts catch
 * reorderings, removals, and additions that shift the last NR. */
static_assert(_IOC_NR(MX_IOCTL_REGISTER_MBOX) == 1,  "MX_IOCTL_* changed — update trace.h + ioctl_to_device");
static_assert(_IOC_NR(MX_IOCTL_HIO_RECV)      == 10, "MX_IOCTL_* changed — update trace.h + ioctl_to_device");

static uint32_t get_pushable_count(struct mx_mbox *mbox)
{
	mbox_index_t head, tail;

	head.full = mbox->ctx.head;
	tail.full = mbox->ctx.tail;

	if (head.phase == tail.phase)
		return mbox->depth - tail.index;
	else
		return head.index - tail.index;
}

static uint32_t get_popable_count(struct mx_mbox *mbox)
{
	mbox_index_t head, tail;

	head.full = mbox->ctx.head;
	tail.full = mbox->ctx.tail;

	if (head.phase == tail.phase)
		return tail.index - head.index;
	else
		return mbox->depth - head.index;
}

/* Returns ERR_PTR on failure so the caller can propagate the real errno
 * (invalid context vs. failed read vs. OOM). */
static struct mx_mbox *create_mx_mbox(struct mx_pci_dev *mx_pdev, uint64_t ctx_addr, uint64_t data_addr)
{
	struct device *dev = &mx_pdev->pdev->dev;
	struct mx_mbox *mbox;
	uint64_t ctx;
	ssize_t ret;

	ret = read_ctrl_from_device(mx_pdev, (char __user *)&ctx, sizeof(uint64_t), (loff_t *)&ctx_addr, IO_OPCODE_SQ_READ);
	if (ret <= 0)
		return ERR_PTR(ret < 0 ? ret : -EIO);

	if (ctx == ULLONG_MAX) {
		pr_info("Invalid mbox context (ctx_addr = 0x%llx)\n", ctx_addr);
		return ERR_PTR(-EINVAL);
	}

	mbox = devm_kzalloc(dev, sizeof(struct mx_mbox), GFP_KERNEL);
	if (!mbox)
		return ERR_PTR(-ENOMEM);

	mx_mbox_init(mbox, ctx_addr, data_addr, ctx);

	return mbox;
}

static int reset_mx_mbox(struct mx_pci_dev *mx_pdev, struct mx_mbox *mbox)
{
	uint64_t ctx;
	ssize_t ret;

	ret = read_ctrl_from_device(mx_pdev, (char __user *)&ctx, sizeof(uint64_t), (loff_t *)&mbox->r_ctx_addr, IO_OPCODE_SQ_READ);
	if (ret <= 0)
		return ret < 0 ? ret : -EIO;

	mbox->ctx.u64 = ctx;

	return 0;
}

static struct mx_mbox *get_sq_mbox(struct mx_pci_dev *mx_pdev, uint32_t qid)
{
	return (qid < MAX_NUM_OF_MBOX) ? mx_pdev->sq_mbox_list[qid] : NULL;
}

static struct mx_mbox *get_cq_mbox(struct mx_pci_dev *mx_pdev, uint32_t qid)
{
	return (qid < MAX_NUM_OF_MBOX) ? mx_pdev->cq_mbox_list[qid] : NULL;
}

/* The ctx address fixes the mailbox's hardware context (data_addr is derived from
 * it), so matching SQ+CQ ctx addresses identify the same registered mailbox. */
static bool registered_mbox_matches(struct mx_pci_dev *mx_pdev, uint32_t qid,
				    const struct mx_ioctl_mbox_info *info)
{
	return mx_pdev->sq_mbox_list[qid]->r_ctx_addr == info->sq_ctx_addr &&
	       mx_pdev->cq_mbox_list[qid]->r_ctx_addr == info->cq_ctx_addr;
}

static long ioctl_register_mbox(struct mx_pci_dev *mx_pdev, unsigned long arg)
{
	struct mx_ioctl_mbox_info mbox_info;
	struct mx_mbox *sq_mbox, *cq_mbox;

	if (copy_from_user(&mbox_info, (void __user *)arg, sizeof(mbox_info)))
		return -EFAULT;

	if (mbox_info.qid >= MAX_NUM_OF_MBOX)
		return -EINVAL;

	/* The qid owned by the driver's internal HIO channel is never host-registerable;
	 * its submit/complete threads drive that hardware context unconditionally. */
	if (mx_pdev->reserved_hio_qid >= 0 && mbox_info.qid == (uint32_t)mx_pdev->reserved_hio_qid)
		return -EBUSY;

	mutex_lock(&mx_pdev->bar_mmap_lock);
	/* Mutually exclusive with an actively mapped BAR: a mapped BAR lets userspace drive
	 * the mailbox region, so a kernel mailbox would double-own it. mapping_mapped() reads
	 * live VMAs, so registration reopens once userspace munmaps the BAR. */
	if (mx_pdev->mmap_mapping && mapping_mapped(mx_pdev->mmap_mapping)) {
		mutex_unlock(&mx_pdev->bar_mmap_lock);
		return -EBUSY;
	}
	/* Idempotent only for the same context: a populated slot with matching SQ+CQ ctx
	 * addresses is a no-op success; a mismatch repoints a qid that has no unregister
	 * path, so reject it. An SQ slot implies its CQ (populated together). */
	if (mx_pdev->sq_mbox_list[mbox_info.qid]) {
		bool matches = registered_mbox_matches(mx_pdev, mbox_info.qid, &mbox_info);

		mutex_unlock(&mx_pdev->bar_mmap_lock);
		return matches ? 0 : -EINVAL;
	}
	mutex_unlock(&mx_pdev->bar_mmap_lock);

	sq_mbox = create_mx_mbox(mx_pdev, mbox_info.sq_ctx_addr, mbox_info.sq_data_addr);
	if (IS_ERR(sq_mbox))
		return PTR_ERR(sq_mbox);

	cq_mbox = create_mx_mbox(mx_pdev, mbox_info.cq_ctx_addr, mbox_info.cq_data_addr);
	if (IS_ERR(cq_mbox)) {
		devm_kfree(&mx_pdev->pdev->dev, sq_mbox);
		return PTR_ERR(cq_mbox);
	}

	/* Commit under the lock and re-check: while the mailboxes were built outside
	 * the lock a concurrent mmap may have claimed the BAR, or another thread may
	 * have registered this qid. */
	mutex_lock(&mx_pdev->bar_mmap_lock);
	if (mx_pdev->mmap_mapping && mapping_mapped(mx_pdev->mmap_mapping)) {
		mutex_unlock(&mx_pdev->bar_mmap_lock);
		devm_kfree(&mx_pdev->pdev->dev, cq_mbox);
		devm_kfree(&mx_pdev->pdev->dev, sq_mbox);
		return -EBUSY;
	}
	if (mx_pdev->sq_mbox_list[mbox_info.qid]) {
		bool matches = registered_mbox_matches(mx_pdev, mbox_info.qid, &mbox_info);

		mutex_unlock(&mx_pdev->bar_mmap_lock);
		devm_kfree(&mx_pdev->pdev->dev, cq_mbox);
		devm_kfree(&mx_pdev->pdev->dev, sq_mbox);
		return matches ? 0 : -EINVAL;
	}
	mx_pdev->sq_mbox_list[mbox_info.qid] = sq_mbox;
	mx_pdev->cq_mbox_list[mbox_info.qid] = cq_mbox;
	mutex_unlock(&mx_pdev->bar_mmap_lock);

	return 0;
}

static long ioctl_init_mbox(struct mx_pci_dev *mx_pdev, unsigned long arg)
{
	struct mx_mbox *sq_mbox, *cq_mbox;
	uint32_t qid;
	int ret;

	if (copy_from_user(&qid, (void __user *)arg, sizeof(qid)))
		return -EFAULT;

	sq_mbox = get_sq_mbox(mx_pdev, qid);
	cq_mbox = get_cq_mbox(mx_pdev, qid);
	if (!sq_mbox || !cq_mbox)
		return -EINVAL;

	/* Each reset is an idempotent device-context refresh, so a failed INIT_MBOX
	 * is always safe to retry even if the SQ was already refreshed. */
	ret = reset_mx_mbox(mx_pdev, sq_mbox);
	if (ret)
		return ret;

	ret = reset_mx_mbox(mx_pdev, cq_mbox);
	if (ret)
		return ret;

	return 0;
}

static long ioctl_send_cmd_with_data(struct mx_pci_dev *mx_pdev, unsigned long arg)
{
	struct mx_ioctl_cmd_with_data send_cmd;
	struct mx_mbox *sq_mbox;
	uint64_t data_addr;

	if (copy_from_user(&send_cmd, (void __user *)arg, sizeof(send_cmd)))
		return -EFAULT;

	sq_mbox = get_sq_mbox(mx_pdev, send_cmd.qid);
	if (!sq_mbox)
		return -EINVAL;

	if (send_cmd.user_addr && send_cmd.size > 0)
		traced_write_data(mx_pdev, send_cmd.qid, send_cmd.user_addr, send_cmd.size,
				&send_cmd.device_addr, IO_OPCODE_DATA_WRITE, true);

	mutex_lock(&sq_mbox->lock);
	while (is_full(sq_mbox)) {
		mbox_context_t ctx;

		if (traced_read_ctrl(mx_pdev, send_cmd.qid, (char __user *)&ctx.u64, sizeof(uint64_t),
				(loff_t *)&sq_mbox->r_ctx_addr, IO_OPCODE_SQ_READ) <= 0) {
			mutex_unlock(&sq_mbox->lock);
			return -EINTR;
		}
		sq_mbox->ctx.head = ctx.head;
	}

	data_addr = sq_mbox->data_addr + get_data_offset(sq_mbox->ctx.tail);
	sq_mbox->ctx.tail = get_next_index(sq_mbox->ctx.tail, 1, sq_mbox->depth);

	traced_write_data(mx_pdev, send_cmd.qid, (const char __user *)send_cmd.cmd, sizeof(uint64_t),
			(loff_t *)&data_addr, IO_OPCODE_CONTEXT_WRITE, true);
	traced_write_ctrl(mx_pdev, send_cmd.qid, (const char __user *)&sq_mbox->ctx.u64, sizeof(uint64_t),
			(loff_t *)&sq_mbox->w_ctx_addr, IO_OPCODE_SQ_WRITE, true);
	mutex_unlock(&sq_mbox->lock);

	return 0;
}

static long ioctl_send_cmds(struct mx_pci_dev *mx_pdev, unsigned long arg)
{
	struct mx_ioctl_cmds send_cmd;
	struct mx_mbox *sq_mbox;
	uint64_t data_addr;
	uint32_t count = 0;

	if (copy_from_user(&send_cmd, (void __user *)arg, sizeof(send_cmd)))
		return -EFAULT;

	sq_mbox = get_sq_mbox(mx_pdev, send_cmd.qid);
	if (!sq_mbox)
		return -EINVAL;

	mutex_lock(&sq_mbox->lock);

	/*
	 * Cached head only lags the real device head (device advances head
	 * monotonically), so cached_pushable <= real_pushable. Skip the PCIe
	 * read when the cache already covers the request; fall through otherwise.
	 */
	count = get_pushable_count(sq_mbox);
	if (count < send_cmd.nr_cmds) {
		mbox_context_t ctx;

		if (traced_read_ctrl(mx_pdev, send_cmd.qid, (char __user *)&ctx.u64, sizeof(uint64_t),
				(loff_t *)&sq_mbox->r_ctx_addr, IO_OPCODE_SQ_READ) <= 0) {
			mutex_unlock(&sq_mbox->lock);
			return -EINTR;
		}
		sq_mbox->ctx.head = ctx.head;
		count = get_pushable_count(sq_mbox);
	}

	if (count == 0)
		goto out;

	if (count > send_cmd.nr_cmds)
		count = send_cmd.nr_cmds;

	data_addr = sq_mbox->data_addr + get_data_offset(sq_mbox->ctx.tail);
	sq_mbox->ctx.tail = get_next_index(sq_mbox->ctx.tail, count, sq_mbox->depth);

	traced_write_data(mx_pdev, send_cmd.qid, (const char __user *)send_cmd.cmds,
			sizeof(uint64_t) * count, (loff_t *)&data_addr, IO_OPCODE_CONTEXT_WRITE, true);
	traced_write_ctrl(mx_pdev, send_cmd.qid, (const char __user *)&sq_mbox->ctx.u64, sizeof(uint64_t),
			(loff_t *)&sq_mbox->w_ctx_addr, IO_OPCODE_SQ_WRITE, true);

out:
	mutex_unlock(&sq_mbox->lock);

	send_cmd.nr_cmds = count;
	if (copy_to_user((void __user *)arg, &send_cmd, sizeof(send_cmd)))
		return -EFAULT;

	return 0;
}

static long ioctl_recv_cmds(struct mx_pci_dev *mx_pdev, unsigned long arg)
{
	struct mx_ioctl_cmds recv_cmd;
	struct mx_mbox *cq_mbox;
	mbox_context_t ctx;
	uint64_t data_addr;
	uint32_t count = 0;

	if (copy_from_user(&recv_cmd, (void __user *)arg, sizeof(recv_cmd)))
		return -EFAULT;

	cq_mbox = get_cq_mbox(mx_pdev, recv_cmd.qid);
	if (!cq_mbox)
		return -EINVAL;

	if (recv_cmd.nr_cmds == 0 || !recv_cmd.cmds)
		return -EINVAL;

	mutex_lock(&cq_mbox->lock);
	if (traced_read_ctrl(mx_pdev, recv_cmd.qid, (char __user *)&ctx.u64, sizeof(uint64_t),
			(loff_t *)&cq_mbox->r_ctx_addr, IO_OPCODE_CQ_READ) <= 0) {
		mutex_unlock(&cq_mbox->lock);
		return -EINTR;
	}
	cq_mbox->ctx.tail = ctx.tail;

	if (is_empty(cq_mbox))
		goto out;

	count = get_popable_count(cq_mbox);
	if (count > recv_cmd.nr_cmds)
		count = recv_cmd.nr_cmds;

	data_addr = cq_mbox->data_addr + get_data_offset(cq_mbox->ctx.head);
	cq_mbox->ctx.head = get_next_index(cq_mbox->ctx.head, count, cq_mbox->depth);

	traced_read_data(mx_pdev, recv_cmd.qid, (char __user *)recv_cmd.cmds, count * sizeof(uint64_t),
			(loff_t *)&data_addr, IO_OPCODE_CONTEXT_READ);
	traced_write_ctrl(mx_pdev, recv_cmd.qid, (const char __user *)&cq_mbox->ctx.u64, sizeof(uint64_t),
			(loff_t *)&cq_mbox->w_ctx_addr, IO_OPCODE_CQ_WRITE, true);

out:
	mutex_unlock(&cq_mbox->lock);

	recv_cmd.nr_cmds = count;
	if (copy_to_user((void __user *)arg, &recv_cmd, sizeof(recv_cmd)))
		return -EFAULT;

	return 0;
}

static long ioctl_read_data(struct mx_pci_dev *mx_pdev, unsigned long arg)
{
	struct mx_ioctl_data read_data;
	ssize_t ret;

	if (copy_from_user(&read_data, (void __user *)arg, sizeof(read_data)))
		return -EFAULT;

	if (read_data.size == 0 || !read_data.user_addr)
		return -EINVAL;

	ret = read_data_from_device_parallel(mx_pdev, (char __user *)read_data.user_addr, read_data.size,
			(loff_t *)&read_data.device_addr, IO_OPCODE_DATA_READ);
	if (ret < 0)
		return ret;

	return 0;
}

static long ioctl_write_data(struct mx_pci_dev *mx_pdev, unsigned long arg)
{
	struct mx_ioctl_data write_data;
	ssize_t ret;

	if (copy_from_user(&write_data, (void __user *)arg, sizeof(write_data)))
		return -EFAULT;

	if (write_data.size == 0 || !write_data.user_addr)
		return -EINVAL;

	ret = write_data_to_device_parallel(mx_pdev, (const char __user *)write_data.user_addr, write_data.size,
			(loff_t *)&write_data.device_addr, IO_OPCODE_DATA_WRITE, write_data.no_wait);
	if (ret < 0)
		return ret;

	return 0;
}

static long ioctl_passthru_cmd(struct mx_pci_dev *mx_pdev, unsigned long arg)
{
	struct mx_ioctl_passthru_cmd cmd;
	long ret;

	if (copy_from_user(&cmd, (void __user *)arg, sizeof(cmd)))
		return -EFAULT;

	if (cmd.subopcode > 0xF)
		return -EINVAL;

	ret = submit_passthru_command(mx_pdev, cmd.subopcode,
				      cmd.device_addr, cmd.size, cmd.no_completion,
				      &cmd.status, &cmd.host_addr);
	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &cmd, sizeof(cmd)))
		return -EFAULT;

	return 0;
}

static long ioctl_hio_protocol(struct mx_pci_dev *mx_pdev, unsigned long arg, int opcode)
{
	struct mx_ioctl_protocol_cmd cmd;

	if (copy_from_user(&cmd, (void __user *)arg, sizeof(cmd)))
		return -EFAULT;

	if (!cmd.buf)
		return -EINVAL;

	return submit_protocol_transfer(mx_pdev, cmd.buf, cmd.size, opcode);
}

long ioctl_to_device(struct mx_pci_dev *mx_pdev, unsigned int cmd, unsigned long arg)
{
	long ret;

	/* Setup-only ioctls bypass tracing — they fire once per mailbox lifetime. */
	switch (cmd) {
	case MX_IOCTL_REGISTER_MBOX:
		return ioctl_register_mbox(mx_pdev, arg);
	case MX_IOCTL_INIT_MBOX:
		return ioctl_init_mbox(mx_pdev, arg);
	}

	trace_mx_dma_ioctl_enter(mx_pdev->dev_id, cmd);

	switch (cmd) {
	case MX_IOCTL_SEND_CMD_WITH_DATA:
		ret = ioctl_send_cmd_with_data(mx_pdev, arg);
		break;
	case MX_IOCTL_RECV_CMDS:
		ret = ioctl_recv_cmds(mx_pdev, arg);
		break;
	case MX_IOCTL_SEND_CMDS:
		ret = ioctl_send_cmds(mx_pdev, arg);
		break;
	case MX_IOCTL_READ_DATA:
		ret = ioctl_read_data(mx_pdev, arg);
		break;
	case MX_IOCTL_WRITE_DATA:
		ret = ioctl_write_data(mx_pdev, arg);
		break;
	case MX_IOCTL_PASSTHRU_CMD:
		ret = ioctl_passthru_cmd(mx_pdev, arg);
		break;
	case MX_IOCTL_HIO_SEND:
		ret = ioctl_hio_protocol(mx_pdev, arg, IO_OPCODE_SEND);
		break;
	case MX_IOCTL_HIO_RECV:
		ret = ioctl_hio_protocol(mx_pdev, arg, IO_OPCODE_RECV);
		break;
	default:
		pr_warn("unknown ioctl cmd(%u)\n", cmd);
		ret = -EINVAL;
		break;
	}

	trace_mx_dma_ioctl_exit(mx_pdev->dev_id, cmd, ret);
	return ret;
}

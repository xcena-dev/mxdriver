// SPDX-License-Identifier: <SPDX License Expression>

#include "mx_dma.h"

#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif

/******************************************************************************/
/* Functions for file_operations                                              */
/******************************************************************************/
static int mxdma_device_open(struct inode *inode, struct file *file)
{
	struct mx_char_dev *mx_cdev;
	struct mx_file_ctx *ctx;

	mx_cdev = container_of(inode->i_cdev, struct mx_char_dev, cdev);
	if (mx_cdev->magic != MAGIC_CHAR) {
		pr_warn("magic is mismatch. mxcdev(0x%p) inode(%#lx)\n", mx_cdev, inode->i_ino);
		return -EINVAL;
	}
	if (!mx_pdev_get_live(mx_cdev->mx_pdev))
		return -ENODEV;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		mx_pdev_put(mx_cdev->mx_pdev);
		return -ENOMEM;
	}
	ctx->magic = MAGIC_FILE_CTX;
	refcount_set(&ctx->refs, 1);
	ctx->mx_cdev = mx_cdev;
	ctx->mx_pdev = mx_cdev->mx_pdev;

	file->private_data = ctx;

	return 0;
}

static int mxdma_device_release(struct inode *inode, struct file *file)
{
	struct mx_file_ctx *ctx = file->private_data;

	if (!ctx || ctx->magic != MAGIC_FILE_CTX) {
		pr_warn("invalid private context of file(0x%p)\n", file);
		return -EINVAL;
	}
	file->private_data = NULL;
	mx_file_ctx_put(ctx);
	return 0;
}

static int mxdma_device_prepare(struct file *file, struct mx_file_ctx **ctx,
				struct mx_char_dev **mx_cdev,
				struct mx_pci_dev **mx_pdev)
{
	*ctx = file->private_data;
	if (!*ctx || (*ctx)->magic != MAGIC_FILE_CTX) {
		pr_warn("invalid private context of file(0x%p)\n", file);
		return -EINVAL;
	}
	*mx_cdev = (*ctx)->mx_cdev;
	if ((*mx_cdev)->magic != MAGIC_CHAR) {
		pr_warn("magic is mismatch. mxcdev(0x%p) file(0x%p)\n", *mx_cdev, file);
		return -EINVAL;
	}

	*mx_pdev = (*ctx)->mx_pdev;
	if (!*mx_pdev) {
		pr_warn("mx_pdev is NULL of file(0x%p)\n", file);
		return -EINVAL;
	}

	if ((*mx_pdev)->magic != MAGIC_DEVICE) {
		pr_warn("magic is mismatch. mx_pdev(0x%p) file(0x%p)\n", *mx_pdev, file);
		return -EINVAL;
	}

	down_read(&(*mx_pdev)->io_rwsem);
	if (READ_ONCE((*mx_pdev)->lease.removed) || !(*mx_pdev)->enabled) {
		pr_warn("pci device isn't enabled. dev_no=%d", (*mx_pdev)->dev_no);
		up_read(&(*mx_pdev)->io_rwsem);
		return -ENODEV;
	}

	return 0;
}

static void mxdma_device_finish(struct mx_pci_dev *mx_pdev)
{
	up_read(&mx_pdev->io_rwsem);
}

static ssize_t mxdma_device_read_data(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	struct mx_file_ctx *ctx;
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	ssize_t ret;

	if (!count) {
		pr_warn("size of data to read is zero\n");
		return -EINVAL;
	}

	if (pos == NULL || *pos == 0) {
		pr_warn("Invalid position to read\n");
		return -EINVAL;
	}

	ret = mxdma_device_prepare(file, &ctx, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;
	ret = mx_lease_direct_begin(ctx);
	if (ret) {
		mxdma_device_finish(mx_pdev);
		return ret;
	}

	mx_prewake_handlers(mx_pdev);
	ret = read_data_from_device_parallel(ctx, buf, count, pos, IO_OPCODE_DATA_READ);
	mx_lease_direct_end(ctx);
	mxdma_device_finish(mx_pdev);
	return ret;
}

static ssize_t mxdma_device_read_context(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	struct mx_file_ctx *ctx;
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	ssize_t ret;

	if (!count) {
		pr_warn("size of data to read is zero\n");
		return -EINVAL;
	}

	if (pos == NULL || *pos == 0) {
		pr_warn("Invalid position to read\n");
		return -EINVAL;
	}

	ret = mxdma_device_prepare(file, &ctx, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;
	ret = mx_lease_direct_begin(ctx);
	if (ret) {
		mxdma_device_finish(mx_pdev);
		return ret;
	}

	mx_prewake_handlers(mx_pdev);
	ret = read_data_from_device(ctx, buf, count, pos, IO_OPCODE_CONTEXT_READ);
	mx_lease_direct_end(ctx);
	mxdma_device_finish(mx_pdev);
	return ret;
}

static ssize_t mxdma_device_write_data(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct mx_file_ctx *ctx;
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	ssize_t ret;

	if (!count) {
		pr_warn("size of data to write is zero\n");
		return -EINVAL;
	}

	ret = mxdma_device_prepare(file, &ctx, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;
	ret = mx_lease_direct_begin(ctx);
	if (ret) {
		mxdma_device_finish(mx_pdev);
		return ret;
	}

	mx_prewake_handlers(mx_pdev);
	ret = write_data_to_device_parallel(ctx, buf, count, pos, IO_OPCODE_DATA_WRITE, false);
	mx_lease_direct_end(ctx);
	mxdma_device_finish(mx_pdev);
	return ret;
}

static ssize_t mxdma_device_write_context(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct mx_file_ctx *ctx;
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	ssize_t ret;

	if (!count) {
		pr_warn("size of data to write is zero\n");
		return -EINVAL;
	}

	ret = mxdma_device_prepare(file, &ctx, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;
	ret = mx_lease_direct_begin(ctx);
	if (ret) {
		mxdma_device_finish(mx_pdev);
		return ret;
	}

	mx_prewake_handlers(mx_pdev);
	ret = write_data_to_device(ctx, buf, count, pos, IO_OPCODE_CONTEXT_WRITE, false);
	mx_lease_direct_end(ctx);
	mxdma_device_finish(mx_pdev);
	return ret;
}

static long mxdma_device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mx_file_ctx *ctx;
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	int ret;

	ret = mxdma_device_prepare(file, &ctx, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;

	if (mx_lease_ioctl_cmd(cmd)) {
		ret = mx_lease_ioctl(ctx, cmd, arg);
	} else if (!mx_ioctl_is_direct_cmd(cmd)) {
		ret = ioctl_to_device(ctx, cmd, arg);
	} else {
		ret = mx_lease_direct_begin(ctx);
		if (!ret) {
			mx_prewake_handlers(mx_pdev);
			ret = ioctl_to_device(ctx, cmd, arg);
			mx_lease_direct_end(ctx);
		}
	}
	mxdma_device_finish(mx_pdev);
	return ret;
}

static long mxdma_lease_only_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	struct mx_file_ctx *ctx;
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	long ret;

	ret = mxdma_device_prepare(file, &ctx, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;
	ret = mx_lease_ioctl_cmd(cmd) ? mx_lease_ioctl(ctx, cmd, arg) : -ENOTTY;
	mxdma_device_finish(mx_pdev);
	return ret;
}

#ifdef CONFIG_COMPAT
static long mxdma_device_compat_ioctl(struct file *file, unsigned int cmd,
				      unsigned long arg)
{
	if (!mx_lease_ioctl_cmd(cmd))
		return -ENOIOCTLCMD;
	return mxdma_device_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}

static long mxdma_lease_only_compat_ioctl(struct file *file, unsigned int cmd,
					 unsigned long arg)
{
	if (!mx_lease_ioctl_cmd(cmd))
		return -ENOIOCTLCMD;
	return mxdma_lease_only_ioctl(file, cmd,
				       (unsigned long)compat_ptr(arg));
}
#endif

static int mxdma_bar_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mx_file_ctx *ctx;
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	int ret;

	ret = mxdma_device_prepare(file, &ctx, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;

	ret = mx_lease_direct_begin(ctx);
	if (!ret) {
		ret = mx_pdev->ops.bar_mmap ?
			mx_pdev->ops.bar_mmap(mx_pdev, ctx, vma) : -EOPNOTSUPP;
		mx_lease_direct_end(ctx);
	}
	mxdma_device_finish(mx_pdev);
	return ret;
}

static __poll_t mxdma_device_poll(struct file *file, poll_table *wait)
{
	struct mx_file_ctx *ctx;
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	struct mx_event *mx_event;
	int ret;

	ret = mxdma_device_prepare(file, &ctx, &mx_cdev, &mx_pdev);
	if (ret)
		return ret == -ENODEV ? EPOLLERR | EPOLLHUP : EPOLLERR;

	mx_event = &mx_pdev->event;
	poll_wait(file, &mx_event->wq, wait);

	ret = atomic_read(&mx_event->count);
	if (ret > 0) {
		atomic_dec(&mx_event->count);
		mxdma_device_finish(mx_pdev);
		return EPOLLIN | EPOLLRDNORM;
	}
	if (READ_ONCE(mx_pdev->lease.removed)) {
		mxdma_device_finish(mx_pdev);
		return EPOLLERR | EPOLLHUP;
	}

	mxdma_device_finish(mx_pdev);
	return 0;
}

static ssize_t mxdma_bdf_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct mx_file_ctx *ctx;
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	char bdf_str[32];
	int len;
	ssize_t ret;

	ret = mxdma_device_prepare(file, &ctx, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;
	len = scnprintf(bdf_str, sizeof(bdf_str), "%s\n", mx_pdev->bdf);
	ret = simple_read_from_buffer(buf, count, ppos, bdf_str, len);
	mxdma_device_finish(mx_pdev);
	return ret;
}

static const struct file_operations mxdma_fops_data = {
	.owner = THIS_MODULE,
	.open = mxdma_device_open,
	.release = mxdma_device_release,
	.read = mxdma_device_read_data,
	.write = mxdma_device_write_data,
	.unlocked_ioctl = mxdma_lease_only_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mxdma_lease_only_compat_ioctl,
#endif
};

static const struct file_operations mxdma_fops_context = {
	.owner = THIS_MODULE,
	.open = mxdma_device_open,
	.release = mxdma_device_release,
	.read = mxdma_device_read_context,
	.write = mxdma_device_write_context,
	.unlocked_ioctl = mxdma_lease_only_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mxdma_lease_only_compat_ioctl,
#endif
};

static const struct file_operations mxdma_fops_ioctl = {
	.owner = THIS_MODULE,
	.open = mxdma_device_open,
	.release = mxdma_device_release,
	.unlocked_ioctl = mxdma_device_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mxdma_device_compat_ioctl,
#endif
	.mmap = mxdma_bar_mmap,
};

static const struct file_operations mxdma_fops_event = {
	.owner = THIS_MODULE,
	.open = mxdma_device_open,
	.release = mxdma_device_release,
	.poll = mxdma_device_poll,
};

static const struct file_operations mxdma_fops_bdf = {
	.owner = THIS_MODULE,
	.open = mxdma_device_open,
	.release = mxdma_device_release,
	.read = mxdma_bdf_read,
};

const struct file_operations *mxdma_fops_array[] = {
	[MX_CDEV_DATA] = &mxdma_fops_data,
	[MX_CDEV_CONTEXT] = &mxdma_fops_context,
	[MX_CDEV_IOCTL] = &mxdma_fops_ioctl,
	[MX_CDEV_EVENT] = &mxdma_fops_event,
	[MX_CDEV_BDF] = &mxdma_fops_bdf,
};

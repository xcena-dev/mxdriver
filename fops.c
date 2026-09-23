// SPDX-License-Identifier: <SPDX License Expression>

#include "mx_dma.h"

/******************************************************************************/
/* Functions for file_operations                                              */
/******************************************************************************/
static int mxdma_device_open(struct inode *inode, struct file *file)
{
	struct mx_char_dev *mx_cdev;

	mx_cdev = container_of(inode->i_cdev, struct mx_char_dev, cdev);
	if (mx_cdev->magic != MAGIC_CHAR) {
		pr_warn("magic is mismatch. mxcdev(0x%p) inode(%#lx)\n", mx_cdev, inode->i_ino);
		return -EINVAL;
	}

	/* cdev_device_del() does not stop chrdev_open() on an inode that already carries i_cdev,
	 * so refuse new files once the device is going away. */
	if (percpu_ref_is_dying(&mx_cdev->mx_pdev->io_ref))
		return -ENODEV;

	file->private_data = mx_cdev;

	return 0;
}

static int mxdma_device_release(struct inode *inode, struct file *file)
{
	struct mx_char_dev *mx_cdev;

	mx_cdev = (struct mx_char_dev *)file->private_data;
	if (!mx_cdev) {
		pr_warn("mx_cdev is NULL of file(0x%p)\n", file);
		return -EINVAL;
	}

	if (mx_cdev->magic != MAGIC_CHAR) {
		pr_warn("magic is mismatch. mxcdev(0x%p) file(0x%p)\n", mx_cdev, file);
		return -EINVAL;
	}

	file->private_data = NULL;

	return 0;
}

/* Every fops body runs between a successful get and its put;
 * teardown kills io_ref and waits for zero before touching what the body uses. */
static int mxdma_device_get(struct file *file, struct mx_char_dev **mx_cdev, struct mx_pci_dev **mx_pdev)
{
	*mx_cdev = (struct mx_char_dev *)file->private_data;
	if (!*mx_cdev) {
		pr_warn("mx_cdev is NULL of file(0x%p)\n", file);
		return -EINVAL;
	}

	if ((*mx_cdev)->magic != MAGIC_CHAR) {
		pr_warn("magic is mismatch. mxcdev(0x%p) file(0x%p)\n", *mx_cdev, file);
		return -EINVAL;
	}

	*mx_pdev = (*mx_cdev)->mx_pdev;
	if (!*mx_pdev) {
		pr_warn("mx_pdev is NULL of file(0x%p)\n", file);
		return -EINVAL;
	}

	if ((*mx_pdev)->magic != MAGIC_DEVICE) {
		pr_warn("magic is mismatch. mx_pdev(0x%p) file(0x%p)\n", *mx_pdev, file);
		return -EINVAL;
	}

	if (!percpu_ref_tryget_live(&(*mx_pdev)->io_ref))
		return -ENODEV;

	return 0;
}

static void mxdma_device_put(struct mx_pci_dev *mx_pdev)
{
	percpu_ref_put(&mx_pdev->io_ref);
}

static ssize_t mxdma_device_read_data(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
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

	ret = mxdma_device_get(file, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;

	mx_prewake_handlers(mx_pdev);
	ret = read_data_from_device_parallel(mx_pdev, buf, count, pos, IO_OPCODE_DATA_READ);
	mxdma_device_put(mx_pdev);
	return ret;
}

static ssize_t mxdma_device_read_context(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
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

	ret = mxdma_device_get(file, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;

	mx_prewake_handlers(mx_pdev);
	ret = read_data_from_device(mx_pdev, buf, count, pos, IO_OPCODE_CONTEXT_READ);
	mxdma_device_put(mx_pdev);
	return ret;
}

static ssize_t mxdma_device_write_data(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	ssize_t ret;

	if (!count) {
		pr_warn("size of data to write is zero\n");
		return -EINVAL;
	}

	ret = mxdma_device_get(file, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;

	mx_prewake_handlers(mx_pdev);
	ret = write_data_to_device_parallel(mx_pdev, buf, count, pos, IO_OPCODE_DATA_WRITE, false);
	mxdma_device_put(mx_pdev);
	return ret;
}

static ssize_t mxdma_device_write_context(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	ssize_t ret;

	if (!count) {
		pr_warn("size of data to write is zero\n");
		return -EINVAL;
	}

	ret = mxdma_device_get(file, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;

	mx_prewake_handlers(mx_pdev);
	ret = write_data_to_device(mx_pdev, buf, count, pos, IO_OPCODE_CONTEXT_WRITE, false);
	mxdma_device_put(mx_pdev);
	return ret;
}

static long mxdma_device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	long ret;

	ret = mxdma_device_get(file, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;

	mx_prewake_handlers(mx_pdev);
	ret = ioctl_to_device(mx_pdev, cmd, arg);
	mxdma_device_put(mx_pdev);
	return ret;
}

static int mxdma_bar_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	int ret;

	ret = mxdma_device_get(file, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;

	ret = mx_bar_mmap(mx_pdev, vma);
	mxdma_device_put(mx_pdev);
	return ret;
}

static __poll_t mxdma_device_poll(struct file *file, poll_table *wait)
{
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	struct mx_event *mx_event;
	__poll_t mask = 0;

	if (mxdma_device_get(file, &mx_cdev, &mx_pdev))
		return EPOLLERR;

	mx_event = &mx_pdev->event;
	poll_wait(file, &mx_event->wq, wait);

	/* The kill and its wake-up may both predate our poll_wait() registration. */
	if (percpu_ref_is_dying(&mx_pdev->io_ref)) {
		mask = EPOLLERR;
	} else if (atomic_read(&mx_event->count) > 0) {
		atomic_dec(&mx_event->count);
		mask = EPOLLIN | EPOLLRDNORM;
	}

	mxdma_device_put(mx_pdev);
	return mask;
}

static ssize_t mxdma_bdf_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	char bdf_str[32];
	ssize_t ret;
	int len;

	ret = mxdma_device_get(file, &mx_cdev, &mx_pdev);
	if (ret)
		return ret;

	len = scnprintf(bdf_str, sizeof(bdf_str), "%s\n", dev_name(&mx_pdev->pdev->dev));
	ret = simple_read_from_buffer(buf, count, ppos, bdf_str, len);
	mxdma_device_put(mx_pdev);
	return ret;
}

static const struct file_operations mxdma_fops_data = {
	.owner = THIS_MODULE,
	.open = mxdma_device_open,
	.release = mxdma_device_release,
	.read = mxdma_device_read_data,
	.write = mxdma_device_write_data,
};

static const struct file_operations mxdma_fops_context = {
	.owner = THIS_MODULE,
	.open = mxdma_device_open,
	.release = mxdma_device_release,
	.read = mxdma_device_read_context,
	.write = mxdma_device_write_context,
};

static const struct file_operations mxdma_fops_ioctl = {
	.owner = THIS_MODULE,
	.open = mxdma_device_open,
	.release = mxdma_device_release,
	.unlocked_ioctl = mxdma_device_ioctl,
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

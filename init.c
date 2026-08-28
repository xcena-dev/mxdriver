// SPDX-License-Identifier: <SPDX License Expression>

#include "mx_dma.h"

/******************************************************************************/
/* Initialization                                                             */
/******************************************************************************/
static struct class *mxdma_class;
struct kmem_cache *mx_transfer_cache;

static LIST_HEAD(mx_device_list_head);
static DEFINE_MUTEX(mx_device_list_lock);
static bool mxdma_accepting_devices;
#ifndef CONFIG_WO_CXL
static bool mxdma_module_coming;
#endif

static void mx_event_init(struct mx_pci_dev *mx_pdev)
{
	struct mx_event *mx_event = &mx_pdev->event;

	init_waitqueue_head(&mx_event->wq);
	atomic_set(&mx_event->count, 0);
}

static irqreturn_t msi_irq_handler(int irq, void *data)
{
	struct mx_pci_dev *mx_pdev;
	struct mx_event *mx_event;

	mx_pdev = (struct mx_pci_dev *)data;
	if (!mx_pdev) {
		pr_err("Invalid data\n");
		goto out;
	}

	mx_event = &(mx_pdev->event);
	if (!mx_event) {
		pr_err("Invalid event\n");
		goto out;
	}

	atomic_inc(&mx_event->count);
	wake_up_interruptible(&mx_event->wq);

out:
	return IRQ_HANDLED;
}

static void pci_device_exit(struct mx_pci_dev* mx_pdev)
{
	struct pci_dev *pdev = mx_pdev->pdev;

	if (mx_pdev->irq_requested) {
		free_irq(pci_irq_vector(pdev, 0), mx_pdev);
		mx_pdev->irq_requested = false;
	}
	if (mx_pdev->msi_enabled_by_driver) {
		pci_disable_msi(pdev);
		mx_pdev->msi_enabled_by_driver = false;
	}
	if (mx_pdev->readrq_changed) {
		if (pcie_set_readrq(pdev, mx_pdev->saved_readrq))
			dev_warn(&pdev->dev, "failed to restore PCIe read request size\n");
		mx_pdev->readrq_changed = false;
	}
	if (mx_pdev->min_align_mask_changed) {
		/* Ignore the pre-6.12 return value as in the setup path: a bound
		 * PCI device has dma_parms, and the helper is void on newer kernels. */
		dma_set_min_align_mask(&pdev->dev,
				       mx_pdev->saved_min_align_mask);
		mx_pdev->min_align_mask_changed = false;
	}
	if (mx_pdev->max_seg_size_changed) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0) || \
	RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 6)
		/* dma_set_max_seg_size() returns void since 6.10 and in RHEL 9.6. */
		dma_set_max_seg_size(&pdev->dev, mx_pdev->saved_max_seg_size);
#else
		if (dma_set_max_seg_size(&pdev->dev,
					 mx_pdev->saved_max_seg_size))
			dev_warn(&pdev->dev, "failed to restore DMA max segment size\n");
#endif
		mx_pdev->max_seg_size_changed = false;
	}
	if (mx_pdev->coherent_dma_mask_changed) {
		if (dma_set_coherent_mask(&pdev->dev,
					  mx_pdev->saved_coherent_dma_mask))
			dev_warn(&pdev->dev,
				 "failed to restore coherent DMA mask\n");
		mx_pdev->coherent_dma_mask_changed = false;
	}
	if (mx_pdev->dma_mask_changed) {
		if (dma_set_mask(&pdev->dev, mx_pdev->saved_dma_mask))
			dev_warn(&pdev->dev, "failed to restore streaming DMA mask\n");
		mx_pdev->dma_mask_changed = false;
	}
	if (mx_pdev->bus_master_enabled_by_driver) {
		pci_clear_master(pdev);
		mx_pdev->bus_master_enabled_by_driver = false;
	}
	if (mx_pdev->pci_enabled_by_driver) {
		pci_disable_device(pdev);
		mx_pdev->pci_enabled_by_driver = false;
	}
}

static int pci_device_init(struct mx_pci_dev* mx_pdev)
{
	struct pci_dev *pdev = mx_pdev->pdev;
	int ret;

	if (pci_is_enabled(pdev) == false) {
#ifdef CONFIG_WO_CXL
		ret = pci_enable_device_mem(pdev);
		if (ret) {
			pr_err("Failed to pci_enable_device (err=%d)\n", ret);
			return ret;
		}
		mx_pdev->pci_enabled_by_driver = true;
#else
		/* In notifier mode the bound CXL driver owns PCI enablement. Attaching
		 * devres or enabling the function behind its back is not reversible. */
		return -ENODEV;
#endif
	}

	mx_pdev->saved_readrq = pcie_get_readrq(pdev);
	if (mx_pdev->saved_readrq < 0)
		return mx_pdev->saved_readrq;
	if (mx_pdev->saved_readrq != PAGE_SIZE) {
		ret = pcie_set_readrq(pdev, PAGE_SIZE);
		if (ret) {
			pr_err("Failed to pcie_set_readrq (err=%d)\n", ret);
			return ret;
		}
		mx_pdev->readrq_changed = true;
	}

	if (!pdev->is_busmaster) {
#ifdef CONFIG_WO_CXL
		pci_set_master(pdev);
		mx_pdev->bus_master_enabled_by_driver = true;
#else
		return -ENODEV;
#endif
	}

	if (pci_dev_msi_enabled(pdev) == false) {
		ret = pci_enable_msi(pdev);
		if (ret) {
			pr_err("Failed to pci_enable_msi (err=%d)\n", ret);
			return ret;
		}
		mx_pdev->msi_enabled_by_driver = true;
	}

	int irq = pci_irq_vector(pdev, 0);
	if (irq < 0) {
		pr_err("Failed to get msi irq vector (err=%d)\n", irq);
		return -ENODEV;
	}

	ret = request_threaded_irq(irq, msi_irq_handler, NULL, 0, MXDMA_NODE_NAME, mx_pdev);
	if (ret) {
		pr_err("Failed to request_threaded_irq (err=%d)\n", ret);
		return ret;
	}
	mx_pdev->irq_requested = true;

	return 0;
}

static void dev_unmap(struct mx_pci_dev *mx_pdev)
{
	struct pci_dev *pdev = mx_pdev->pdev;

	if (mx_pdev->bar) {
		pci_iounmap(pdev, mx_pdev->bar);
		mx_pdev->bar = NULL;
	}

	if (mx_pdev->bar_region_requested) {
		pci_release_region(pdev, MXDMA_BAR_INDEX);
		mx_pdev->bar_region_requested = false;
	}
}

static int dev_map(struct mx_pci_dev *mx_pdev)
{
	struct pci_dev *pdev = mx_pdev->pdev;
	resource_size_t size;
	int ret;

	ret = pci_request_region(pdev, MXDMA_BAR_INDEX, MXDMA_NODE_NAME);
	if (ret) {
		pr_err("Failed to pci_request_region (err=%d)\n", ret);
		return ret;
	}
	mx_pdev->bar_region_requested = true;

	size = pci_resource_len(pdev, MXDMA_BAR_INDEX);
	mx_pdev->bar = pci_iomap(pdev, MXDMA_BAR_INDEX, size);
	if (!mx_pdev->bar) {
		pr_err("Failed to pci_iomap (size=%llu)\n",
				(unsigned long long)size);
		pci_release_region(pdev, MXDMA_BAR_INDEX);
		mx_pdev->bar_region_requested = false;
		return -ENOMEM;
	}

	mx_pdev->bar_mapped_size = size;

	return 0;
}

static int set_dma_addressing(struct mx_pci_dev *mx_pdev)
{
	struct pci_dev *pdev = mx_pdev->pdev;

#ifndef CONFIG_WO_CXL
	u64 selected_dma_mask;
	unsigned int required_min_align_mask;

	/* The CXL driver owns dev->dma_mask and DMA parameters. Its configured
	 * mask is also honored by our explicit dma_alloc/map calls. Preserve and
	 * restore every foreign DMA parameter that this notifier overlay must
	 * tighten for the MX PRP format. */
	if (!pdev->dev.dma_mask || !*pdev->dev.dma_mask)
		return -EINVAL;
	mx_pdev->saved_dma_mask = *pdev->dev.dma_mask;
	mx_pdev->saved_coherent_dma_mask = pdev->dev.coherent_dma_mask;
	if (!dma_set_mask(&pdev->dev, DMA_BIT_MASK(48))) {
		selected_dma_mask = DMA_BIT_MASK(48);
	} else if (!dma_set_mask(&pdev->dev, DMA_BIT_MASK(32))) {
		selected_dma_mask = DMA_BIT_MASK(32);
	} else {
		return -EINVAL;
	}
	mx_pdev->dma_mask_changed =
		selected_dma_mask != mx_pdev->saved_dma_mask;
	if (dma_set_coherent_mask(&pdev->dev, selected_dma_mask))
		return -EINVAL;
	mx_pdev->coherent_dma_mask_changed =
		selected_dma_mask != mx_pdev->saved_coherent_dma_mask;

	mx_pdev->saved_max_seg_size = dma_get_max_seg_size(&pdev->dev);
	if (mx_pdev->saved_max_seg_size > SZ_1G) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0) || \
	RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 6)
		/* dma_set_max_seg_size() returns void since 6.10 and in RHEL 9.6. */
		dma_set_max_seg_size(&pdev->dev, SZ_1G);
#else
		int ret = dma_set_max_seg_size(&pdev->dev, SZ_1G);

		if (ret)
			return ret;
#endif
		mx_pdev->max_seg_size_changed = true;
	}

	/* PRP entries carry addresses but no lengths. Preserve intra-page offsets
	 * when the DMA API uses bounce buffers so all non-final SG entries remain
	 * page-boundary expressible, matching the standalone path below. */
	mx_pdev->saved_min_align_mask = dma_get_min_align_mask(&pdev->dev);
	required_min_align_mask = mx_pdev->saved_min_align_mask |
				  (PAGE_SIZE - 1);
	if (required_min_align_mask != mx_pdev->saved_min_align_mask) {
		/* The helper returns void from 6.12 onward. A bound PCI device has
		 * dma_parms, so the older return value cannot report failure here. */
		dma_set_min_align_mask(&pdev->dev, required_min_align_mask);
		mx_pdev->min_align_mask_changed = true;
	}
	return 0;
#else
	/* 48-bit addressing capability for MXDMA? */
	if (!dma_set_mask(&pdev->dev, DMA_BIT_MASK(48))) {
		/* use 48-bit DMA */
		pr_info("use 48-bit DMA\n");
		if (dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(48)))
			return -EINVAL;
	} else if (!dma_set_mask(&pdev->dev, DMA_BIT_MASK(32))) {
		/* use 32-bit DMA */
		pr_info("use 32-bit DMA\n");
		if (dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32)))
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	/* scatterlist::dma_length is unsigned int — a single coalesced DMA segment
	 * exactly at 4 GiB wraps to 0 and breaks length-based SG walks (e.g.
	 * mx_sg_locate).  Cap so dma_map_sg never produces a 32-bit-overflowing entry. */
	dma_set_max_seg_size(&pdev->dev, SZ_1G);

	/* PRP carries no lengths, so the device splits chunks by DMA address; SG entries must end
	 * on chunk boundaries like the pinned user pages do.  Bounce buffers only keep that true
	 * if they preserve intra-page offsets, so require it as NVMe does. */
	/* Return value discarded on purpose: it only reports a NULL dev->dma_parms, which
	 * pci_device_add() always fills in, and the helper returns void from 6.12 on. */
	dma_set_min_align_mask(&pdev->dev, PAGE_SIZE - 1);

	return 0;
#endif
}

static ssize_t liveness_enable_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct mx_pci_dev *mx_pdev = dev_get_drvdata(dev);

	if (!mx_pdev)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", READ_ONCE(mx_pdev->liveness_enable));
}

static ssize_t liveness_enable_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct mx_pci_dev *mx_pdev = dev_get_drvdata(dev);
	struct mx_queue *q;
	unsigned long flags;
	bool val;
	int ret;

	if (!mx_pdev)
		return -ENODEV;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	/* Reset watchdog state and publish the toggle in one sq_lock section;
	 * the submit handler samples the flag before locking, so publish-first
	 * could run the watchdog on stale state and false-DEAD a live transfer. */
	q = mx_pdev->io_queue;
	if (q) {
		spin_lock_irqsave(&q->sq_lock, flags);
		WRITE_ONCE(mx_pdev->liveness_enable, val);
		atomic_set(&q->lv_inflight, 0);
		/* Zero, not keep: rtt_ns is only rewritten on a pong, so a re-enable
		 * would otherwise expose the previous session's RTT indefinitely. */
		WRITE_ONCE(q->lv_rtt_ns, 0);
		if (val) {
			WRITE_ONCE(q->lv_progress_jiffies, jiffies);
			atomic_set(&q->lv_health, MX_LIVENESS_ALIVE);
		} else {
			atomic_set(&q->lv_health, MX_LIVENESS_UNKNOWN);
		}
		spin_unlock_irqrestore(&q->sq_lock, flags);
	} else {
		WRITE_ONCE(mx_pdev->liveness_enable, val);
	}

	return count;
}
static DEVICE_ATTR(enable, 0644, liveness_enable_show, liveness_enable_store);

/* stall_ms/dead_ms/max_mult share one shape: a per-device uint. Store only
 * parses; the watchdog and transfer read sites sanitize (1 <= stall < dead,
 * mult ceiling), so no bound is enforced here. */
#define LIVENESS_UINT_ATTR(attr_name, field)					\
static ssize_t attr_name##_show(struct device *dev,				\
				struct device_attribute *attr, char *buf)	\
{										\
	struct mx_pci_dev *mx_pdev = dev_get_drvdata(dev);			\
	if (!mx_pdev)								\
		return -ENODEV;							\
	return sysfs_emit(buf, "%u\n", READ_ONCE(mx_pdev->field));		\
}										\
static ssize_t attr_name##_store(struct device *dev,				\
				 struct device_attribute *attr,			\
				 const char *buf, size_t count)			\
{										\
	struct mx_pci_dev *mx_pdev = dev_get_drvdata(dev);			\
	unsigned int val;							\
	int ret;								\
	if (!mx_pdev)								\
		return -ENODEV;							\
	ret = kstrtouint(buf, 0, &val);						\
	if (ret)								\
		return ret;							\
	WRITE_ONCE(mx_pdev->field, val);					\
	return count;								\
}										\
static DEVICE_ATTR(attr_name, 0644, attr_name##_show, attr_name##_store)

LIVENESS_UINT_ATTR(stall_ms, liveness_stall_ms);
LIVENESS_UINT_ATTR(dead_ms, liveness_dead_ms);
LIVENESS_UINT_ATTR(max_mult, liveness_max_mult);

static const char * const liveness_health_name[] = {
	[MX_LIVENESS_UNKNOWN] = "unknown",
	[MX_LIVENESS_ALIVE]   = "alive",
	[MX_LIVENESS_SUSPECT] = "suspect",
	[MX_LIVENESS_DEAD]    = "dead",
};

/* State attrs report the sentinel (unknown / 0) whenever the watchdog is off or
 * the io_queue is gone, never a stale value. */
static ssize_t health_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct mx_pci_dev *mx_pdev = dev_get_drvdata(dev);
	struct mx_queue *q;
	int health = MX_LIVENESS_UNKNOWN;

	if (!mx_pdev)
		return -ENODEV;
	q = mx_pdev->io_queue;
	if (READ_ONCE(mx_pdev->liveness_enable) && q)
		health = atomic_read(&q->lv_health);
	return sysfs_emit(buf, "%s\n", liveness_health_name[health]);
}
static DEVICE_ATTR_RO(health);

static ssize_t rtt_ns_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct mx_pci_dev *mx_pdev = dev_get_drvdata(dev);
	struct mx_queue *q;
	u64 rtt = 0;

	if (!mx_pdev)
		return -ENODEV;
	q = mx_pdev->io_queue;
	if (READ_ONCE(mx_pdev->liveness_enable) && q)
		rtt = READ_ONCE(q->lv_rtt_ns);
	return sysfs_emit(buf, "%llu\n", rtt);
}
static DEVICE_ATTR_RO(rtt_ns);

static struct attribute *liveness_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_stall_ms.attr,
	&dev_attr_dead_ms.attr,
	&dev_attr_max_mult.attr,
	&dev_attr_health.attr,
	&dev_attr_rtt_ns.attr,
	NULL,
};

static const struct attribute_group liveness_group = {
	.name = "liveness",
	.attrs = liveness_attrs,
};

static const struct attribute_group *mxdma_dev_groups[] = {
	&liveness_group,
	NULL,
};

static void mx_cdev_device_release(struct device *dev)
{
	struct mx_char_dev *mx_cdev = container_of(dev, struct mx_char_dev, device);

	mx_pdev_put(mx_cdev->mx_pdev);
}

static int create_mx_cdev(struct mx_pci_dev *mx_pdev, int type)
{
	struct mx_char_dev *mx_cdev = &mx_pdev->mx_cdev[type];
	int ret;

	mx_cdev->magic = MAGIC_CHAR;
	mx_cdev->mx_pdev = mx_pdev;
	mx_cdev->type = type;
	mx_cdev->cdev_no = MKDEV(MAJOR(mx_pdev->dev_no), mx_pdev->num_of_cdev++);

	cdev_init(&mx_cdev->cdev, mxdma_fops_array[type]);
	kref_get(&mx_pdev->refcount);
	device_initialize(&mx_cdev->device);
	mx_cdev->device.class = mxdma_class;
	mx_cdev->device.parent = &mx_pdev->pdev->dev;
	mx_cdev->device.devt = mx_cdev->cdev_no;
	mx_cdev->device.release = mx_cdev_device_release;
	if (type == MX_CDEV_IOCTL) {
		mx_cdev->device.groups = mxdma_dev_groups;
		dev_set_drvdata(&mx_cdev->device, mx_pdev);
	}
	ret = dev_set_name(&mx_cdev->device, node_name[type], mx_pdev->dev_id);
	if (ret)
		goto out_put_device;

	/* The class device owns one mx_pdev reference. cdev_device_add() makes
	 * the cdev a child of that device, so an open racing cdev_device_del()
	 * keeps both the embedded cdev and mx_pdev alive until its cdev ref drops. */
	ret = cdev_device_add(&mx_cdev->cdev, &mx_cdev->device);
	if (ret) {
		pr_err("Failed to cdev_device_add (err=%d)\n", ret);
		goto out_put_device;
	}

	mx_cdev->enabled = true;

	pr_info("%s (%d:%d) is created\n", dev_name(&mx_cdev->device),
			MAJOR(mx_cdev->cdev_no), MINOR(mx_cdev->cdev_no));

	return 0;

out_put_device:
	put_device(&mx_cdev->device);
	return ret;
}

static void destroy_mx_cdev(struct mx_char_dev *mx_cdev)
{
	if (!mx_cdev->enabled)
		return;
	mx_cdev->enabled = false;

	pr_info("%s (%d:%d) is destroyed\n", dev_name(&mx_cdev->device),
			MAJOR(mx_cdev->cdev_no), MINOR(mx_cdev->cdev_no));

	cdev_device_del(&mx_cdev->cdev, &mx_cdev->device);
	put_device(&mx_cdev->device);
}

static void mxdma_device_online(struct mx_pci_dev *mx_pdev)
{
	mutex_lock(&mx_pdev->bar_mmap_lock);
	mx_pdev->enabled = true;
	mutex_unlock(&mx_pdev->bar_mmap_lock);
}

static void mxdma_device_offline(struct mx_pci_dev *mx_pdev)
{
	mutex_lock(&mx_pdev->bar_mmap_lock);
	mx_pdev->enabled = false;
	mutex_unlock(&mx_pdev->bar_mmap_lock);
}

static void mxdma_unmap_all_bar_vmas(struct mx_pci_dev *mx_pdev)
{
	struct mx_bar_vma *bar_vma;

	mutex_lock(&mx_pdev->bar_mmap_lock);
	/* unmap_mapping_range() zaps PTEs but does not close the VMA, so vm_close
	 * cannot re-enter this lock. The bar_vma's mx_pdev reference remains until
	 * the last VMA (including fork copies) closes. */
	list_for_each_entry(bar_vma, &mx_pdev->bar_mappings, entry)
		unmap_mapping_range(bar_vma->mapping, 0, 0, 1);
	mutex_unlock(&mx_pdev->bar_mmap_lock);
}

static bool mxdma_io_queue_idle(struct mx_pci_dev *mx_pdev)
{
	struct mx_queue *queue = mx_pdev->io_queue;
	unsigned long flags;
	bool sq_empty;

	if (!queue)
		return true;
	spin_lock_irqsave(&queue->sq_lock, flags);
	sq_empty = list_empty(&queue->sq_list);
	spin_unlock_irqrestore(&queue->sq_lock, flags);
	return sq_empty && atomic_read(&queue->wait_count) == 0 &&
	       atomic_read(&queue->zombie_wait_count) == 0;
}

static bool mxdma_pci_permanently_gone(struct mx_pci_dev *mx_pdev)
{
	return pci_dev_is_disconnected(mx_pdev->pdev);
}

static int mxdma_stop_device_protocol(struct mx_pci_dev *mx_pdev,
				      bool actual_pci_unbind)
{
	unsigned int log_ticks = 0;
	int ret = 0;

	/* nowait waiters may outlive the issuing syscall. Drain them before
	 * inspecting queue counts; each finishes normally or parks a DMA-pinned
	 * zombie that makes the device fail closed below. */
	if (mx_pdev->async_wq) {
		destroy_workqueue(mx_pdev->async_wq);
		mx_pdev->async_wq = NULL;
	}

	if (actual_pci_unbind) {
		/* A normal unbind must not tear down DMA ops while a device command can
		 * still resume. Keep completion processing alive without a timeout. A
		 * wedged device intentionally blocks unbind; reboot/reset is the safe
		 * recovery, not releasing pages under an ambiguous command. */
		while (!mxdma_io_queue_idle(mx_pdev)) {
			if (mxdma_pci_permanently_gone(mx_pdev)) {
				mx_stop_queue_threads(mx_pdev);
				return 0;
			}
			swake_up_one(&mx_pdev->io_queue->sq_wait);
			swake_up_one(&mx_pdev->io_queue->cq_wait);
			if (++log_ticks % 50 == 0)
				dev_err(&mx_pdev->pdev->dev,
					"waiting for MXDMA commands to reach a terminal state before unbind\n");
			msleep(100);
		}
	} else if (!mxdma_io_queue_idle(mx_pdev)) {
		WRITE_ONCE(mx_pdev->protocol_poisoned, true);
		mx_stop_queue_threads(mx_pdev);
		return -EBUSY;
	}

	if (!actual_pci_unbind && READ_ONCE(mx_pdev->protocol_poisoned)) {
		mx_stop_queue_threads(mx_pdev);
		return -EUCLEAN;
	}
	while (actual_pci_unbind && READ_ONCE(mx_pdev->protocol_poisoned)) {
		if (mxdma_pci_permanently_gone(mx_pdev)) {
			mx_stop_queue_threads(mx_pdev);
			return 0;
		}
		ret = mx_pdev->ops.recover_queue ?
			mx_pdev->ops.recover_queue(mx_pdev) : -EOPNOTSUPP;
		if (!ret)
			break;
		dev_err_ratelimited(&mx_pdev->pdev->dev,
			"draining ambiguous MXDMA admin command before unbind\n");
		msleep(100);
	}
	if (!mx_pdev->queues_initialized && mx_pdev->queue_dma_programmed) {
		ret = mx_pdev->ops.recover_queue ?
			mx_pdev->ops.recover_queue(mx_pdev) : -EOPNOTSUPP;
		if (ret && !actual_pci_unbind) {
			WRITE_ONCE(mx_pdev->protocol_poisoned, true);
			return ret;
		}
		while (ret) {
			if (mxdma_pci_permanently_gone(mx_pdev))
				return 0;
			dev_err_ratelimited(&mx_pdev->pdev->dev,
				"disabling partial MXDMA queue before unbind\n");
			msleep(100);
			ret = mx_pdev->ops.recover_queue ?
				mx_pdev->ops.recover_queue(mx_pdev) :
				-EOPNOTSUPP;
		}
	}

	if (mx_pdev->queues_initialized) {
		ret = mx_pdev->ops.release_queue(mx_pdev);
		if (ret) {
			WRITE_ONCE(mx_pdev->protocol_poisoned, true);
			if (!actual_pci_unbind)
				return ret;
			while (ret) {
				if (mxdma_pci_permanently_gone(mx_pdev))
					return 0;
				ret = mx_pdev->ops.recover_queue ?
					mx_pdev->ops.recover_queue(mx_pdev) :
					-EOPNOTSUPP;
				if (ret) {
					dev_err_ratelimited(&mx_pdev->pdev->dev,
						"recovering MXDMA queue deletion before unbind\n");
					msleep(100);
				}
			}
		}
		if (!ret)
			mx_pdev->queues_initialized = false;
	} else {
		mx_stop_queue_threads(mx_pdev);
	}

	return READ_ONCE(mx_pdev->protocol_poisoned) ? -EIO : ret;
}

static int mxdma_fence_pci_unbind(struct mx_pci_dev *mx_pdev)
{
	struct pci_dev *pdev = mx_pdev->pdev;

	/* This is legal only from the real PCI/CXL unbind path. Protocol proof
	 * above establishes that no internal command can resume after a later
	 * rebind; clearing Bus Master only fences already-issued PCI transactions. */
	pci_clear_master(pdev);
	while (!pci_wait_for_pending_transaction(pdev)) {
		if (mxdma_pci_permanently_gone(mx_pdev))
			return 0;
		dev_err_ratelimited(&pdev->dev,
			"waiting for pending PCI transactions before MXDMA teardown\n");
		msleep(100);
	}
	return 0;
}

/* Returns true only when every DMA-visible allocation was safely released.
 * A false return intentionally leaves queue/page/transfer DMA memory pinned,
 * the registry entry live, and the overlay module self-reference held. */
static bool destroy_mx_pdev(struct mx_pci_dev *mx_pdev, bool actual_pci_unbind)
{
	bool safe;
	int type;

	if (mx_pdev->teardown_started) {
		if (!mx_pdev->protocol_poisoned)
			return true;
		if (!actual_pci_unbind)
			return false;
		/* Attach failed after an ambiguous admin command. Its BAR/admin backing
		 * was deliberately retained; the real unbind can now drain the late
		 * completion and delete any queue it actually created. */
		down_write(&mx_pdev->io_rwsem);
		goto stop_protocol;
	}
	mx_pdev->teardown_started = true;
	mx_lease_mark_removed(mx_pdev);
	down_write(&mx_pdev->io_rwsem);
	mxdma_device_offline(mx_pdev);
	wake_up_interruptible_poll(&mx_pdev->event.wq, EPOLLERR | EPOLLHUP);

	for (type = 0; type < NUM_OF_MX_CDEV; type++)
		destroy_mx_cdev(&mx_pdev->mx_cdev[type]);
	mxdma_unmap_all_bar_vmas(mx_pdev);
	if (cpu_latency_qos_request_active(&mx_pdev->cpu_latency_req))
		cpu_latency_qos_remove_request(&mx_pdev->cpu_latency_req);

stop_protocol:
	safe = !mxdma_stop_device_protocol(mx_pdev, actual_pci_unbind);
	if (safe && actual_pci_unbind)
		safe = !mxdma_fence_pci_unbind(mx_pdev);

	WRITE_ONCE(mx_pdev->dma_reclaim_safe, safe);
	if (safe && !IS_ERR_OR_NULL(mx_pdev->zombie_cleanup_thread)) {
		if (kthread_stop(mx_pdev->zombie_cleanup_thread) < 0)
			pr_err("Failed to stop zombie_cleanup_thread\n");
		mx_pdev->zombie_cleanup_thread = NULL;
	}

	if (mx_pdev->chrdev_region_allocated) {
		unregister_chrdev_region(mx_pdev->dev_no, NUM_OF_MX_CDEV);
		mx_pdev->chrdev_region_allocated = false;
	}

	if (!safe) {
		mx_pdev->attach_error = -EUCLEAN;
		dev_crit(&mx_pdev->pdev->dev,
			 "MXDMA protocol state is ambiguous; BAR and DMA state retained until the owning driver unbinds\n");
		up_write(&mx_pdev->io_rwsem);
		return false;
	}

	/* Only after protocol proof may BAR/config and DMA allocations be released. */
	mx_free_registered_mboxes(mx_pdev);
	if (mx_pdev->page_pool) {
		dma_pool_destroy(mx_pdev->page_pool);
		mx_pdev->page_pool = NULL;
	}
	if (mx_pdev->ops.free_queue)
		mx_pdev->ops.free_queue(mx_pdev);
	dev_unmap(mx_pdev);
	pci_device_exit(mx_pdev);
	mx_pdev->pdev = NULL;
	up_write(&mx_pdev->io_rwsem);
	return true;
}

static int create_mx_pdev(struct pci_dev *pdev, int cxl_memdev_id,
			  struct mx_pci_dev **out_pdev)
{
	struct mx_pci_dev *mx_pdev;
	int type;
	int ret;

	mx_pdev = kzalloc(sizeof(*mx_pdev), GFP_KERNEL);
	if (!mx_pdev)
		return -ENOMEM;
	*out_pdev = mx_pdev;

	mx_lease_init(mx_pdev);
	mx_event_init(mx_pdev);
	mx_pdev->magic = MAGIC_DEVICE;
	mx_pdev->pdev = pdev;
	mx_pdev->dev_id = cxl_memdev_id;
	strscpy(mx_pdev->bdf, dev_name(&pdev->dev), sizeof(mx_pdev->bdf));
	mx_pdev->liveness_stall_ms = LIVENESS_STALL_MS_DEFAULT;
	mx_pdev->liveness_dead_ms = LIVENESS_DEAD_MS_DEFAULT;
	mx_pdev->liveness_max_mult = LIVENESS_MAX_MULT_DEFAULT;
	mx_pdev->reserved_hio_qid = -1;
	INIT_LIST_HEAD(&mx_pdev->registry_entry);
	mutex_init(&mx_pdev->bar_mmap_lock);
	INIT_LIST_HEAD(&mx_pdev->bar_mappings);
	INIT_LIST_HEAD(&mx_pdev->zombie_list);
	spin_lock_init(&mx_pdev->zombie_lock);

	if (pdev->revision == 0x1) {
		register_mx_ops_v1(&mx_pdev->ops);
		pr_info("PCI device revision 1 detected\n");
	} else if (pdev->revision == 0x2) {
		register_mx_ops_v2(&mx_pdev->ops);
		pr_info("PCI device revision 2 detected\n");
	} else {
		pr_err("Unknown PCI device revision %d\n", pdev->revision);
		return -EINVAL;
	}

	cpu_latency_qos_add_request(&mx_pdev->cpu_latency_req,
				    MX_CPU_LATENCY_QOS_US);
	ret = alloc_chrdev_region(&mx_pdev->dev_no, 0, NUM_OF_MX_CDEV,
				  MXDMA_NODE_NAME);
	if (ret)
		return ret;
	mx_pdev->chrdev_region_allocated = true;

	ret = dev_map(mx_pdev);
	if (ret)
		return ret;
	ret = pci_device_init(mx_pdev);
	if (ret)
		return ret;
	ret = set_dma_addressing(mx_pdev);
	if (ret)
		return ret;
	ret = mx_pdev->ops.init_queue(mx_pdev);
	if (ret)
		return ret;
	mx_pdev->queues_initialized = true;

	mx_pdev->async_wq = alloc_workqueue("mx_dma_wait%d",
					    WQ_UNBOUND | WQ_MEM_RECLAIM, 0,
					    mx_pdev->dev_id);
	if (!mx_pdev->async_wq)
		return -ENOMEM;
	mx_pdev->zombie_cleanup_thread = kthread_run(zombie_cleanup_handler,
			mx_pdev, "mx_zombie_cleanup_thd%d", mx_pdev->dev_id);
	if (IS_ERR(mx_pdev->zombie_cleanup_thread)) {
		ret = PTR_ERR(mx_pdev->zombie_cleanup_thread);
		mx_pdev->zombie_cleanup_thread = NULL;
		return ret;
	}

	/* Publish cdevs only after every queue, worker, and DMA pool exists. */
	mx_pdev->page_pool = dma_pool_create("mxdma_page_pool", &pdev->dev,
			mx_pdev->page_size, mx_pdev->page_size, 0);
	if (!mx_pdev->page_pool)
		return -ENOMEM;
	for (type = 0; type < NUM_OF_MX_CDEV; type++) {
		ret = create_mx_cdev(mx_pdev, type);
		if (ret)
			return ret;
	}
	mxdma_device_online(mx_pdev);
	return 0;
}

/******************************************************************************/
/* PCI Device Driver Support                                                  */
/******************************************************************************/
static const struct pci_device_id pci_ids[] = {
	{ PCI_DEVICE(XCENA_PCI_VENDOR_ID, PCI_ANY_ID), },
	{ 0,}
};
MODULE_DEVICE_TABLE(pci, pci_ids);

#ifndef CONFIG_WO_CXL
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0) && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 6)
static int match_mem_prefix(struct device *dev, void *data)
#else
static int match_mem_prefix(struct device *dev, const void *data)
#endif
{
	const char *name;

	name = dev_name(dev);
	return name && !strncmp(name, MXDMA_MEM_NAME, MEM_NAME_LEN);
}
#endif

static int get_cxl_memdev_id(struct pci_dev *pdev)
{
#ifdef CONFIG_WO_CXL
	static int standalone_id = -1;
	return ++standalone_id;
#else
	int mem_id;
	struct device *child;

	child = device_find_child(&pdev->dev, NULL, match_mem_prefix);
	if (!child)
	{
		pr_err("No matching CXL memory device found for PCI device %s.\n", dev_name(&pdev->dev));
		return -ENODEV;
	}

	if (sscanf(dev_name(child), MXDMA_MEM_NAME "%d", &mem_id) != 1 || mem_id < 0)
	{
		pr_err("Failed to parse CXL memory device ID from device name %s.\n", dev_name(child));
		mem_id = -ENODEV;
	}

	put_device(child);
	return mem_id;
#endif
}

static struct mx_pci_dev *mxdma_find_device_locked(struct pci_dev *pdev)
{
	struct mx_pci_dev *mx_pdev;

	list_for_each_entry(mx_pdev, &mx_device_list_head, registry_entry) {
		if (mx_pdev->pdev == pdev)
			return mx_pdev;
	}
	return NULL;
}

static void mxdma_drop_attachment(struct mx_pci_dev *mx_pdev,
				  struct pci_dev *pdev)
{
	bool module_ref_held = mx_pdev->module_ref_held;
	bool pci_ref_held = mx_pdev->pci_ref_held;

	mx_pdev->module_ref_held = false;
	mx_pdev->pci_ref_held = false;
	mx_pdev_put(mx_pdev);
	if (pci_ref_held)
		pci_dev_put(pdev);
	if (module_ref_held)
		module_put(THIS_MODULE);
}

static int mxdma_attach_device(struct pci_dev *pdev)
{
	struct mx_pci_dev *mx_pdev = NULL;
#ifndef CONFIG_WO_CXL
	bool module_coming;
#endif
	int cxl_memdev_id;
	int ret;

	mutex_lock(&mx_device_list_lock);
	if (!mxdma_accepting_devices) {
		ret = -ESHUTDOWN;
		mutex_unlock(&mx_device_list_lock);
		return ret;
	}
	mx_pdev = mxdma_find_device_locked(pdev);
	if (mx_pdev) {
		ret = mx_pdev->protocol_poisoned ? -EUCLEAN : 0;
		mutex_unlock(&mx_device_list_lock);
		return ret;
	}
#ifndef CONFIG_WO_CXL
	module_coming = mxdma_module_coming;
#endif
	mutex_unlock(&mx_device_list_lock);

	cxl_memdev_id = get_cxl_memdev_id(pdev);
	if (cxl_memdev_id < 0)
		return cxl_memdev_id;

	/* Pin both code and the physical pci_dev before any queue/MMIO programming.
	 * Normal rmmod is therefore EBUSY while an overlay exists; users must first
	 * unbind the owning PCI/CXL driver so this notifier can prove teardown. */
	/* A notifier overlay has no pci_driver owner reference, so explicitly pin
	 * it from pre-programming through CXL UNBIND. The standalone PCI driver
	 * must retain normal rmmod -> pci_unregister_driver -> .remove semantics. */
#ifndef CONFIG_WO_CXL
	if (module_coming)
		__module_get(THIS_MODULE);
	else if (!try_module_get(THIS_MODULE))
		return -ENODEV;
#endif
	pci_dev_get(pdev);
	ret = create_mx_pdev(pdev, cxl_memdev_id, &mx_pdev);
	if (!mx_pdev) {
		pci_dev_put(pdev);
#ifndef CONFIG_WO_CXL
		module_put(THIS_MODULE);
#endif
		return ret;
	}
#ifndef CONFIG_WO_CXL
	mx_pdev->module_ref_held = true;
#endif
	mx_pdev->pci_ref_held = true;
	if (ret) {
		mx_pdev->attach_error = ret;
		if (destroy_mx_pdev(mx_pdev, false)) {
			mxdma_drop_attachment(mx_pdev, pdev);
			return ret;
		}
		mutex_lock(&mx_device_list_lock);
		list_add_tail(&mx_pdev->registry_entry, &mx_device_list_head);
		mutex_unlock(&mx_device_list_lock);
		dev_crit(&pdev->dev,
			 "MXDMA attach failed after ambiguous hardware programming; device pinned unavailable until reset/reboot\n");
#ifdef CONFIG_WO_CXL
		/* Keep the PCI function bound to us so no other driver can re-enable
		 * bus mastering against retained DMA state. */
		ret = 0;
#endif
		return ret;
	}

	mutex_lock(&mx_device_list_lock);
	list_add_tail(&mx_pdev->registry_entry, &mx_device_list_head);
	mutex_unlock(&mx_device_list_lock);
	pr_info("pci device is attached (vendor=%#x device=%#x bdf=%s cxl=mem%d)\n",
		pdev->vendor, pdev->device, dev_name(&pdev->dev), cxl_memdev_id);
	return 0;
}

static void mxdma_detach_device(struct pci_dev *pdev)
{
	struct mx_pci_dev *mx_pdev;

	mutex_lock(&mx_device_list_lock);
	mx_pdev = mxdma_find_device_locked(pdev);
	mutex_unlock(&mx_device_list_lock);
	if (!mx_pdev)
		return;
	if (!destroy_mx_pdev(mx_pdev, true))
		return;
	mutex_lock(&mx_device_list_lock);
	if (!list_empty(&mx_pdev->registry_entry))
		list_del_init(&mx_pdev->registry_entry);
	mutex_unlock(&mx_device_list_lock);
	pr_info("pci device is detached (vendor=%#x device=%#x bdf=%s)\n",
		pdev->vendor, pdev->device, dev_name(&pdev->dev));
	mxdma_drop_attachment(mx_pdev, pdev);
}

#ifdef CONFIG_WO_CXL
static int __mxdma_driver_probe(struct pci_dev *pdev,
				const struct pci_device_id *id)
{
	return mxdma_attach_device(pdev);
}

static void __mxdma_driver_remove(struct pci_dev *pdev)
{
	mxdma_detach_device(pdev);
}

static struct pci_driver pci_driver = {
	.name		= MXDMA_NODE_NAME,
	.id_table	= pci_ids,
	.probe		= __mxdma_driver_probe,
	.remove		= __mxdma_driver_remove,
};
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 6) && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 6)
static char *mxdma_devnode(struct device *dev, umode_t *mode)
#else
static char *mxdma_devnode(const struct device *dev, umode_t *mode)
#endif
{
	if (mode)
		*mode = 0666;
	return kasprintf(GFP_KERNEL, "%s/%s", MXDMA_NODE_NAME, dev_name(dev));
}

#ifndef CONFIG_WO_CXL
static bool mxdma_is_bound_cxl_pci(struct pci_dev *pdev)
{
	return pdev->dev.driver && pdev->dev.driver->name &&
	       !strcmp(pdev->dev.driver->name, "cxl_pci");
}

static int mxdma_pci_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct pci_dev *pdev;

	pdev = to_pci_dev(data);
	if (pdev->vendor != XCENA_PCI_VENDOR_ID)
		return NOTIFY_OK;

	switch (action) {
	case BUS_NOTIFY_BOUND_DRIVER:
		/* Vendor ID alone is insufficient: vfio-pci and diagnostic drivers
		 * emit the same notifier event but do not provide the CXL lifecycle. */
		if (mxdma_is_bound_cxl_pci(pdev))
			mxdma_attach_device(pdev);
		break;
	case BUS_NOTIFY_UNBIND_DRIVER:
		if (mxdma_is_bound_cxl_pci(pdev))
			mxdma_detach_device(pdev);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block mxdma_pci_notifier = {
	.notifier_call = mxdma_pci_notify,
};

static void mxdma_enumerate_bound_devices(void)
{
	struct pci_dev *pdev = NULL;

	for_each_pci_dev(pdev) {
		int ret = 0;

		if (pdev->vendor != XCENA_PCI_VENDOR_ID)
			continue;
		/* BUS_NOTIFY_{BOUND,UNBIND} run under the device lock. Take the same
		 * lock for init-time enumeration so attach cannot race CXL devres
		 * teardown between the driver check and queue programming. */
		device_lock(&pdev->dev);
		if (mxdma_is_bound_cxl_pci(pdev))
			ret = mxdma_attach_device(pdev);
		device_unlock(&pdev->dev);

		if (ret)
			dev_err(&pdev->dev,
				"failed to attach already-bound CXL device: %d\n",
				ret);
	}
}
#endif

static int mxdma_init(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 3) && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 6)
	mxdma_class = class_create(THIS_MODULE, MXDMA_NODE_NAME);
#else
	mxdma_class = class_create(MXDMA_NODE_NAME);
#endif
	if (IS_ERR(mxdma_class)) {
		pr_err("Failed to class_create (err=%ld)\n", PTR_ERR(mxdma_class));
		return PTR_ERR(mxdma_class);
	}

	mxdma_class->devnode = mxdma_devnode;

	mx_transfer_cache = kmem_cache_create("mx_transfer",
					      sizeof(struct mx_transfer), 0,
					      SLAB_HWCACHE_ALIGN, NULL);
	if (!mx_transfer_cache) {
		pr_err("Failed to create mx_transfer kmem_cache\n");
		class_destroy(mxdma_class);
		return -ENOMEM;
	}

	pr_info("MXDMA driver is loaded\n");
	mutex_lock(&mx_device_list_lock);
	mxdma_accepting_devices = true;
#ifndef CONFIG_WO_CXL
	mxdma_module_coming = true;
#endif
	mutex_unlock(&mx_device_list_lock);

#ifdef CONFIG_WO_CXL
	{
		int ret = pci_register_driver(&pci_driver);

		if (ret) {
			mutex_lock(&mx_device_list_lock);
			mxdma_accepting_devices = false;
			mutex_unlock(&mx_device_list_lock);
			kmem_cache_destroy(mx_transfer_cache);
			mx_transfer_cache = NULL;
			class_destroy(mxdma_class);
		}
		return ret;
	}
#else
	{
		int ret = bus_register_notifier(&pci_bus_type, &mxdma_pci_notifier);

		if (ret) {
			pr_err("Failed to register PCI bus notifier (err=%d)\n", ret);
			mutex_lock(&mx_device_list_lock);
			mxdma_accepting_devices = false;
			mxdma_module_coming = false;
			mutex_unlock(&mx_device_list_lock);
			kmem_cache_destroy(mx_transfer_cache);
			mx_transfer_cache = NULL;
			class_destroy(mxdma_class);
		} else {
			/* Register first, then enumerate. The shared registry deduplicates a
			 * BOUND event racing this walk. */
			mxdma_enumerate_bound_devices();
			mutex_lock(&mx_device_list_lock);
			mxdma_module_coming = false;
			mutex_unlock(&mx_device_list_lock);
		}
		return ret;
	}
#endif
}

static void mxdma_exit(void)
{
	mutex_lock(&mx_device_list_lock);
	mxdma_accepting_devices = false;
#ifndef CONFIG_WO_CXL
	mxdma_module_coming = false;
#endif
	mutex_unlock(&mx_device_list_lock);
#ifdef CONFIG_WO_CXL
	pci_unregister_driver(&pci_driver);
#else
	bus_unregister_notifier(&pci_bus_type, &mxdma_pci_notifier);
#endif
	mutex_lock(&mx_device_list_lock);
	WARN_ON(!list_empty(&mx_device_list_head));
	mutex_unlock(&mx_device_list_lock);

	/*
	 * PCI unregister / device-list teardown above completes all in-flight
	 * transfers (including zombie drain in remove()), so every mx_transfer
	 * has been returned to the slab before we destroy the cache.
	 */
	if (mx_transfer_cache) {
		kmem_cache_destroy(mx_transfer_cache);
		mx_transfer_cache = NULL;
	}

	if (mxdma_class)
		class_destroy(mxdma_class);

	pr_info("MXDMA driver is unloaded\n");
}

module_init(mxdma_init);
module_exit(mxdma_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XCENA Inc.");
MODULE_DESCRIPTION("XCENA MX-DMA Driver");
MODULE_SOFTDEP("post: cxl_pci");

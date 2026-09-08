// SPDX-License-Identifier: <SPDX License Expression>

#include "mx_dma.h"

/******************************************************************************/
/* Initialization                                                             */
/******************************************************************************/
static struct class *mxdma_class;
struct kmem_cache *mx_transfer_cache;

#ifndef CONFIG_WO_CXL
/*
 * In CXL mode cxl_pci is the driver bound to the device, so its driver_data
 * field belongs to cxl_pci.  We ride the PCI bus notifier instead of binding,
 * and keep our own pci_dev -> mx_pci_dev registry here.
 *
 * The registry nodes are ours (kzalloc/kfree), but mx_pci_dev itself and the
 * queues and mailboxes hanging off it still come from devm_* on that foreign
 * device, so their lifetime ends at cxl_pci's devres teardown.  That is safe
 * only because the driver core hands us BUS_NOTIFY_UNBIND_DRIVER before it
 * releases devres, i.e. our teardown always runs first.  Anything added here
 * must keep that ordering in mind until the allocations move to our own
 * lifetime.
 */
static LIST_HEAD(mx_device_list_head);
static DEFINE_MUTEX(mx_device_list_lock);
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

	if (mx_pdev->msi_enabled_by_us) {
		pci_disable_msi(pdev);
		mx_pdev->msi_enabled_by_us = false;
	}
}

static int pci_device_init(struct mx_pci_dev* mx_pdev)
{
	struct pci_dev *pdev = mx_pdev->pdev;
	int ret;

	if (pci_is_enabled(pdev) == false) {
		ret = pcim_enable_device(pdev);
		if (ret) {
			pr_err("Failed to pci_enable_device (err=%d)\n", ret);
			return ret;
		}
	}

	ret = pcie_set_readrq(pdev, PAGE_SIZE);
	if (ret) {
		pr_err("Failed to pcie_set_readrq (err=%d)\n", ret);
		return ret;
	}

	if (!pdev->is_busmaster)
		pci_set_master(pdev);

	if (pci_dev_msi_enabled(pdev) == false) {
		ret = pci_enable_msi(pdev);
		if (ret) {
			pr_err("Failed to pci_enable_msi (err=%d)\n", ret);
			return ret;
		}
		mx_pdev->msi_enabled_by_us = true;
	}

	int irq = pci_irq_vector(pdev, 0);
	if (irq < 0) {
		pr_err("Failed to get msi irq vector (err=%d)\n", irq);
		return -ENODEV;
	}

	ret = request_threaded_irq(irq, msi_irq_handler, NULL, 0, MXDMA_NODE_NAME, mx_pdev);
	if (ret) {
		pr_err("Failed to request_threaded_irq (irq=%d, err=%d)\n", irq, ret);
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

	if (mx_pdev->bar_requested) {
		pci_release_region(pdev, MXDMA_BAR_INDEX);
		mx_pdev->bar_requested = false;
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
	mx_pdev->bar_requested = true;

	size = pci_resource_len(pdev, MXDMA_BAR_INDEX);
	mx_pdev->bar = pci_iomap(pdev, MXDMA_BAR_INDEX, size);
	if (!mx_pdev->bar) {
		pr_err("Failed to pci_iomap (size=%llu)\n",
				(unsigned long long)size);
		return -ENOMEM;
	}

	mx_pdev->bar_mapped_size = size;

	return 0;
}

static int set_dma_addressing(struct pci_dev *pdev)
{
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

static int create_mx_cdev(struct mx_pci_dev *mx_pdev, int type)
{
	struct mx_char_dev *mx_cdev = &mx_pdev->mx_cdev[type];
	struct device *dev;
	int ret;

	mx_cdev->magic = MAGIC_CHAR;
	mx_cdev->cdev_no = MKDEV(MAJOR(mx_pdev->dev_no), mx_pdev->num_of_cdev++);

	cdev_init(&mx_cdev->cdev, mxdma_fops_array[type]);
	kobject_set_name(&mx_cdev->cdev.kobj, node_name[type], mx_pdev->dev_id);

	ret = cdev_add(&mx_cdev->cdev, mx_cdev->cdev_no, 1);
	if (ret) {
		pr_err("Failed to cdev_add (err=%d)\n", ret);
		return ret;
	}

	/* Hang the per-device liveness/ sysfs group off the ioctl node, the device's
	 * control node; drvdata lets its show/store reach mx_pdev. */
	if (type == MX_CDEV_IOCTL)
		dev = device_create_with_groups(mxdma_class, NULL, mx_cdev->cdev_no,
						mx_pdev, mxdma_dev_groups,
						mx_cdev->cdev.kobj.name);
	else
		dev = device_create(mxdma_class, NULL, mx_cdev->cdev_no, NULL, mx_cdev->cdev.kobj.name);
	if (IS_ERR(dev)) {
		pr_err("Failed to device_created (err=%ld)\n", PTR_ERR(dev));
		/* Unregister now: enabled is still false, so destroy_mx_cdev skips this
		 * node and a registered cdev would outlive its devm-freed struct. */
		cdev_del(&mx_cdev->cdev);
		return PTR_ERR(dev);
	}

	mx_cdev->mx_pdev = mx_pdev;
	mx_cdev->enabled = true;

	pr_info("%s (%d:%d) is created\n", mx_cdev->cdev.kobj.name,
			MAJOR(mx_cdev->cdev_no), MINOR(mx_cdev->cdev_no));

	return 0;
}

static void destroy_mx_cdev(struct mx_char_dev *mx_cdev)
{
	if (!mx_cdev->enabled)
		return;
	mx_cdev->enabled = false;

	pr_info("%s (%d:%d) is destroyed\n", mx_cdev->cdev.kobj.name,
			MAJOR(mx_cdev->cdev_no), MINOR(mx_cdev->cdev_no));

	device_destroy(mxdma_class, mx_cdev->cdev_no);
	cdev_del(&mx_cdev->cdev);
}

static void mxdma_device_online(struct mx_pci_dev *mx_pdev)
{
	mx_pdev->enabled = true;
}

static void mxdma_device_offline(struct mx_pci_dev *mx_pdev)
{
	mutex_lock(&mx_pdev->bar_mmap_lock);
	mx_pdev->enabled = false;
	mutex_unlock(&mx_pdev->bar_mmap_lock);
}

static void destroy_mx_pdev(struct mx_pci_dev *mx_pdev)
{
	int type;

	mxdma_device_offline(mx_pdev);

	mutex_lock(&mx_pdev->bar_mmap_lock);
	if (mx_pdev->mmap_mapping) {
		unmap_mapping_range(mx_pdev->mmap_mapping, 0, 0, 1);
		mx_pdev->mmap_mapping = NULL;
	}
	mutex_unlock(&mx_pdev->bar_mmap_lock);

	if (cpu_latency_qos_request_active(&mx_pdev->cpu_latency_req))
		cpu_latency_qos_remove_request(&mx_pdev->cpu_latency_req);

	mx_pdev->ops.release_queue(mx_pdev);

	if (!IS_ERR_OR_NULL(mx_pdev->zombie_cleanup_thread)) {
		if (kthread_stop(mx_pdev->zombie_cleanup_thread) < 0)
			pr_err("Failed to stop zombie_cleanup_thread\n");
	}

	dma_pool_destroy(mx_pdev->page_pool);

	for (type = 0; type < NUM_OF_MX_CDEV; type++)
		destroy_mx_cdev(&mx_pdev->mx_cdev[type]);

	dev_unmap(mx_pdev);
	if (mx_pdev->dev_no)
		unregister_chrdev_region(mx_pdev->dev_no, NUM_OF_MX_CDEV);
	pci_device_exit(mx_pdev);
}

static int create_mx_pdev(struct pci_dev *pdev, int cxl_memdev_id,
			  struct mx_pci_dev **mx_pdev_out)
{
	void (*register_mx_ops)(struct mx_operations *ops);
	struct mx_pci_dev *mx_pdev;
	int type;
	int ret;

	*mx_pdev_out = NULL;

	switch (pdev->revision) {
	case 0x1:
		register_mx_ops = register_mx_ops_v1;
		break;
	case 0x2:
		register_mx_ops = register_mx_ops_v2;
		break;
	default:
		pr_err("Unknown PCI device revision %d\n", pdev->revision);
		return -EINVAL;
	}

	mx_pdev = devm_kzalloc(&pdev->dev, sizeof(struct mx_pci_dev), GFP_KERNEL);
	if (!mx_pdev) {
		pr_err("Failed to alloc mx_pci_dev\n");
		return -ENOMEM;
	}

	mx_pdev->magic = MAGIC_DEVICE;
	mx_pdev->pdev = pdev;
	mx_pdev->dev_id = cxl_memdev_id;
	mx_pdev->liveness_stall_ms = LIVENESS_STALL_MS_DEFAULT;
	mx_pdev->liveness_dead_ms = LIVENESS_DEAD_MS_DEFAULT;
	mx_pdev->liveness_max_mult = LIVENESS_MAX_MULT_DEFAULT;
	mx_pdev->reserved_hio_qid = -1;
	mutex_init(&mx_pdev->bar_mmap_lock);

	register_mx_ops(&mx_pdev->ops);
	pr_info("PCI device revision %d detected\n", pdev->revision);

	/*
	 * Hold a cpu_latency PM QoS for the device's lifetime to block deep C-states whose exit latency would stretch
	 * the freq ramp-up window that adds ~12 us to cold DMA submissions in our measurements.
	 * Acquired after ops registration so every failure below can route through out_fail -> destroy_mx_pdev() for
	 * symmetric cleanup, which needs the release_queue hook in place.
	 */
	cpu_latency_qos_add_request(&mx_pdev->cpu_latency_req, MX_CPU_LATENCY_QOS_US);

	ret = alloc_chrdev_region(&mx_pdev->dev_no, 0, NUM_OF_MX_CDEV, MXDMA_NODE_NAME);
	if (ret) {
		pr_err("Failed to alloc_chrdev_region (err=%d)\n", ret);
		goto out_fail;
	}

	ret = dev_map(mx_pdev);
	if (ret) {
		pr_err("Failed to dev_map (err=%d)\n", ret);
		goto out_fail;
	}

	ret = pci_device_init(mx_pdev);
	if (ret) {
		pr_err("Failed to init_pdev (err=%d)\n", ret);
		goto out_fail;
	}

	ret = set_dma_addressing(pdev);
	if (ret) {
		pr_err("Failed to set_dma_addressing (err=%d)\n", ret);
		goto out_fail;
	}

	ret = mx_pdev->ops.init_queue(mx_pdev);
	if (ret) {
		pr_err("Failed to mx_queue_init (err=%d)\n", ret);
		goto out_fail;
	}

	mx_event_init(mx_pdev);

	INIT_LIST_HEAD(&mx_pdev->zombie_list);
	spin_lock_init(&mx_pdev->zombie_lock);
	mx_pdev->zombie_cleanup_thread = kthread_run(zombie_cleanup_handler, mx_pdev,
			"mx_zombie_cleanup_thd%d", mx_pdev->dev_id);
	if (IS_ERR(mx_pdev->zombie_cleanup_thread)) {
		ret = PTR_ERR(mx_pdev->zombie_cleanup_thread);
		pr_err("Failed to create zombie cleanup thread (err=%d)\n", ret);
		goto out_fail;
	}

	for (type = 0; type < NUM_OF_MX_CDEV; type++) {
		ret = create_mx_cdev(mx_pdev, type);
		if (ret) {
			pr_err("Failed to create mx_cdev (%s) (err=%d)\n", node_name[type], ret);
			goto out_fail;
		}
	}

	mx_pdev->page_pool = dma_pool_create("mxdma_page_pool", &pdev->dev,
			mx_pdev->page_size, mx_pdev->page_size, 0);
	if (!mx_pdev->page_pool) {
		pr_err("Failed to create page_pool\n");
		ret = -ENOMEM;
		goto out_fail;
	}

	mxdma_device_online(mx_pdev);

	*mx_pdev_out = mx_pdev;

	return 0;

out_fail:
	destroy_mx_pdev(mx_pdev);

	return ret;
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

/*
 * Where a probed device's mx_pci_dev is recorded, and how it is found again.
 *
 * Standalone mode binds mx_dma to the device as a PCI driver, so driver_data is
 * ours and holds the pointer directly.  CXL mode has cxl_pci bound instead: its
 * mailbox path reads that same field back as a struct cxl_dev_state, so writing
 * ours there makes cxl_pci read our layout at its own offsets -- a NULL register
 * base and a kernel fault on the next Get Health Info command.  CXL mode
 * therefore leaves driver_data untouched and keeps the pointer in a list of its
 * own, keyed by the struct pci_dev.
 */
#ifndef CONFIG_WO_CXL
static int mx_pdev_register(struct mx_pci_dev *mx_pdev)
{
	struct mx_device_node *mx_node;

	mx_node = kzalloc(sizeof(*mx_node), GFP_KERNEL);
	if (!mx_node)
		return -ENOMEM;
	mx_node->mx_pdev = mx_pdev;

	mutex_lock(&mx_device_list_lock);
	list_add_tail(&mx_node->node, &mx_device_list_head);
	mutex_unlock(&mx_device_list_lock);

	return 0;
}

/* Unregisters the device and hands its state back to be torn down, or returns
 * NULL when we hold none for it.  Unlinking and handing back happen in one lock
 * section, so two callers racing on the same device cannot both take it. */
static struct mx_pci_dev *mx_pdev_unregister(struct pci_dev *pdev)
{
	struct mx_device_node *mx_node;
	struct mx_pci_dev *mx_pdev = NULL;

	mutex_lock(&mx_device_list_lock);
	list_for_each_entry(mx_node, &mx_device_list_head, node) {
		if (mx_node->mx_pdev->pdev == pdev) {
			mx_pdev = mx_node->mx_pdev;
			list_del(&mx_node->node);
			kfree(mx_node);
			break;
		}
	}
	mutex_unlock(&mx_device_list_lock);

	return mx_pdev;
}

/* Same, for whichever device is still registered.  Lets module exit drain the
 * registry through the one teardown path without holding an iterator across a
 * teardown that sleeps. */
static struct mx_pci_dev *mx_pdev_unregister_any(void)
{
	struct mx_device_node *mx_node;
	struct mx_pci_dev *mx_pdev = NULL;

	mutex_lock(&mx_device_list_lock);
	mx_node = list_first_entry_or_null(&mx_device_list_head,
					   struct mx_device_node, node);
	if (mx_node) {
		mx_pdev = mx_node->mx_pdev;
		list_del(&mx_node->node);
		kfree(mx_node);
	}
	mutex_unlock(&mx_device_list_lock);

	return mx_pdev;
}
#else
static int mx_pdev_register(struct mx_pci_dev *mx_pdev)
{
	pci_set_drvdata(mx_pdev->pdev, mx_pdev);
	return 0;
}

static struct mx_pci_dev *mx_pdev_unregister(struct pci_dev *pdev)
{
	struct mx_pci_dev *mx_pdev = pci_get_drvdata(pdev);

	if (mx_pdev)
		pci_set_drvdata(pdev, NULL);

	return mx_pdev;
}
#endif

/* Tears one device down and reports it gone.  The single teardown path: the
 * unbind notification, pci_driver::remove, and the module-exit drain all reach
 * the device through here, after it has left the registry. */
static void remove_mx_pdev(struct mx_pci_dev *mx_pdev)
{
	struct pci_dev *pdev = mx_pdev->pdev;

	destroy_mx_pdev(mx_pdev);

	pr_info("pci device is removed (vendor=%#x, device=%#x, bdf=%s)\n",
			pdev->vendor, pdev->device, dev_name(&pdev->dev));
}

static int __mxdma_driver_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct mx_pci_dev *mx_pdev;
	int ret;
	int cxl_memdev_id;

	cxl_memdev_id = get_cxl_memdev_id(pdev);
	if (cxl_memdev_id < 0)
	{
		pr_err("Failed to get cxl_memdev_id from PCI device %s\n", dev_name(&pdev->dev));
		return -ENODEV;
	}

	ret = create_mx_pdev(pdev, cxl_memdev_id, &mx_pdev);
	if (ret) {
		pr_err("Failed to create_mx_pdev\n");
		return ret;
	}

	ret = mx_pdev_register(mx_pdev);
	if (ret) {
		pr_err("Failed to register mx_pci_dev (err=%d)\n", ret);
		destroy_mx_pdev(mx_pdev);
		return ret;
	}

	pr_info("pci device is probed (vendor=%#x device=%#x bdf=%s cxl=mem%d)\n",
			pdev->vendor, pdev->device, dev_name(&pdev->dev), cxl_memdev_id);

	return 0;
}

static void __mxdma_driver_remove(struct pci_dev *pdev)
{
	struct mx_pci_dev *mx_pdev;

	/* We hold no state for this device: its probe failed, or -- in CXL mode,
	 * where we only attach on the bind notification -- it was already bound
	 * before this module loaded, so we never attached to it at all. */
	mx_pdev = mx_pdev_unregister(pdev);
	if (!mx_pdev)
		return;

	remove_mx_pdev(mx_pdev);
}

#ifdef CONFIG_WO_CXL
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
static int mxdma_pci_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct pci_dev *pdev;

	pdev = to_pci_dev(data);
	if (pdev->vendor != XCENA_PCI_VENDOR_ID)
		return NOTIFY_OK;

	switch (action) {
	case BUS_NOTIFY_BOUND_DRIVER:
		__mxdma_driver_probe(pdev, NULL);
		break;
	case BUS_NOTIFY_UNBIND_DRIVER:
		__mxdma_driver_remove(pdev);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block mxdma_pci_notifier = {
	.notifier_call = mxdma_pci_notify,
};
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

#ifdef CONFIG_WO_CXL
	{
		int ret = pci_register_driver(&pci_driver);

		if (ret) {
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
			kmem_cache_destroy(mx_transfer_cache);
			mx_transfer_cache = NULL;
			class_destroy(mxdma_class);
		}
		return ret;
	}
#endif
}

#ifndef CONFIG_WO_CXL
/* Drains the registry at module exit.  Each device leaves the registry under the
 * lock and is torn down outside it, because the teardown sleeps.  bus_unregister_
 * notifier() has already returned by the time we get here, and it waits out any
 * in-flight chain call, so neither a new entry nor a concurrent teardown of one
 * of these devices is possible. */
static void destroy_device_list(void)
{
	struct mx_pci_dev *mx_pdev;

	while ((mx_pdev = mx_pdev_unregister_any()) != NULL)
		remove_mx_pdev(mx_pdev);
}
#endif

static void mxdma_exit(void)
{
#ifdef CONFIG_WO_CXL
	pci_unregister_driver(&pci_driver);
#else
	bus_unregister_notifier(&pci_bus_type, &mxdma_pci_notifier);
	destroy_device_list();
#endif

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

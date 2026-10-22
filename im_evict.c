// SPDX-License-Identifier: GPL-2.0
/*
 * Infinite Memory eviction notify (PoC), device 0 only.
 * Eviction intents polled over HIO are answered after the kernel parks the host folio;
 * a fault on a parked folio pages it back in with PrefetchSync.
 */
#include "mx_dma.h"

#if IS_ENABLED(CONFIG_IM_SOFT_FAULT)

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/im_soft_fault.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/seq_file.h>
#ifdef CONFIG_X86
#include <asm/cacheflush.h>
#endif

#define IM_HIO_CMD_PAGE		4096
#define IM_HIO_PAYLOAD		(16 * 1024)
#define IM_HIO_BUF_SIZE		(IM_HIO_CMD_PAGE + IM_HIO_PAYLOAD)
#define IM_HIO_GAIA_COMMAND	2
#define IM_GAIA_MAGIC		0x494D
#define IM_GAIA_VERSION		2
#define IM_GAIA_SYSTEM_COMMAND	63
#define IM_SUB_GET_EVICT_EVENTS	12
#define IM_SUB_SET_EVICT_NOTIFY	13
#define IM_SUB_EVICT_REPLY	14
#define IM_PREFETCH_SYNC	8
#define IM_SPN_SHIFT		16
#define IM_SPNS_PER_GROUP	8
#define IM_GROUP_BYTES		((u64)IM_SPNS_PER_GROUP << IM_SPN_SHIFT)
#define IM_EVENTS_VERSION	1
#define IM_DEV_ID		0
#define IM_MAX_RELOCATORS	64
#define IM_MAX_SLOTS		16

/* QEMU host-cache model: writing a line address here writes it back and invalidates it. */
#define IM_QEMU_CACHE_BACKDOOR	0xCE00000000ULL
#define IM_CACHE_LINE		64

struct im_hio_header {
	u16 protocol_opcode;
	u16 flags;
	u32 data_size;
	u32 status;
	u32 reserved0;
	u64 tag;
	u64 reserved1;
} __packed;

struct im_gaia_payload {
	u16 magic;
	u16 version;
	u32 gaia_opcode;
	u64 arg1;
	u64 arg2;
	u8 reserved[40];
} __packed;

struct im_events_header {
	u32 version;
	u32 count;
	u32 relocator_count;
	u32 slots_per_relocator;
} __packed;

struct im_event {
	u8 relocator_id;
	u8 slot_idx;
	u8 generation;
	u8 reserved;
	u32 seq;
	u32 start_spn;
	u32 dpn;
} __packed;

static_assert(sizeof(struct im_hio_header) == 32);
static_assert(sizeof(struct im_gaia_payload) == 64);
static_assert(sizeof(struct im_event) == 16);

static unsigned int im_poll_ms = 5;
module_param(im_poll_ms, uint, 0644);
MODULE_PARM_DESC(im_poll_ms, "Idle interval between eviction-intent polls (ms)");

static unsigned int im_ack_timeout_ms = 5000;
module_param(im_ack_timeout_ms, uint, 0644);
MODULE_PARM_DESC(im_ack_timeout_ms, "Firmware evicts an intent left unanswered this long (ms)");

struct im_ctx {
	/* Serializes enable/disable and every use of the shared HIO buffer. */
	struct mutex lock;
	struct mx_pci_dev *mx_pdev;
	struct im_soft_fault_provider *prov;
	struct task_struct *poller;
	void *buf;
	void __iomem *cache_backdoor;
	/* Debugfs inputs; copied into hpa_base/hpa_size at enable and fixed until disable. */
	u64 hpa_base_param, hpa_size_param;
	u64 hpa_base, hpa_size;
	u64 tag;
	u8 generation;
	bool enabled;
	/* Set under lock before kthread_stop(), whose wakeup would interrupt an HIO wait. */
	bool stopping;
	struct dentry *dir;
	/* The firmware applies a verdict asynchronously, so an answered intent can be listed again. */
	u32 replied_seq[IM_MAX_RELOCATORS][IM_MAX_SLOTS];

	atomic64_t polls, poll_err, events, stale, dup, ack_untracked, ack_parked, ack_already, nack,
		reply_settled, reply_err, fetches, fetch_err;
	atomic64_t poll_ns_max, poll_ns_total, fetch_ns_max, fetch_ns_total, wb_ns_max, wb_ns_total;
};

static struct im_ctx im = {
	.lock = __MUTEX_INITIALIZER(im.lock),
};

static void im_stat_time(atomic64_t *max, atomic64_t *total, u64 ns)
{
	u64 cur = atomic64_read(max);

	while (ns > cur && !atomic64_try_cmpxchg(max, &cur, ns))
		;
	atomic64_add(ns, total);
}

static int im_hio_status_to_errno(u32 status)
{
	switch (status) {
	case 0:
		return 0;
	case 3:		/* InvalidValue: firmware refused the request */
		return -EINVAL;
	case 6:		/* ResourceExhaust */
		return -EBUSY;
	default:
		return -EIO;
	}
}

/* A transfer that timed out or was interrupted may still be written by the device: leak it. */
static void im_hio_abandon_buf(void)
{
	pr_warn("im_evict: HIO transfer did not complete, abandoning its buffer\n");
	im.buf = alloc_pages_exact(IM_HIO_BUF_SIZE, GFP_KERNEL | __GFP_ZERO);
}

static void im_hio_free_buf(void)
{
	if (im.buf)
		free_pages_exact(im.buf, IM_HIO_BUF_SIZE);
	im.buf = NULL;
}

/* Caller holds im.lock. Returns the payload size the firmware wrote, or -errno. */
static int im_hio(int opcode, u32 sub_opcode, u64 value, u32 payload_size)
{
	struct im_hio_header *hdr = im.buf;
	struct im_gaia_payload *payload = im.buf + sizeof(*hdr);
	ssize_t ret;

	if (!im.buf)
		return -ENOMEM;
	memset(im.buf, 0, IM_HIO_CMD_PAGE);
	hdr->protocol_opcode = IM_HIO_GAIA_COMMAND;
	hdr->data_size = payload_size;
	hdr->tag = ++im.tag;
	payload->magic = IM_GAIA_MAGIC;
	payload->version = IM_GAIA_VERSION;
	payload->gaia_opcode = IM_GAIA_SYSTEM_COMMAND;
	payload->arg1 = sub_opcode;
	payload->arg2 = value;

	ret = mx_kxfer_protocol(im.mx_pdev, im.buf, IM_HIO_CMD_PAGE + payload_size, opcode);
	if (ret == -ETIMEDOUT || ret == -EINTR || ret == -EIO)
		im_hio_abandon_buf();
	if (ret < 0)
		return ret;
	if (hdr->tag != im.tag)
		return -EPROTO;
	ret = im_hio_status_to_errno(hdr->status);
	if (ret)
		return ret;
	return min_t(u32, hdr->data_size, payload_size);
}

static void im_writeback(u64 hpa, u64 len)
{
	u64 addr;

	if (im.cache_backdoor) {
		for (addr = hpa; addr < hpa + len; addr += IM_CACHE_LINE)
			writeq(addr, im.cache_backdoor);
		return;
	}
#ifdef CONFIG_X86
	clflush_cache_range(phys_to_virt(hpa), len);
#endif
}

static bool im_event_valid(const struct im_event *ev)
{
	u64 offset = (u64)ev->start_spn << IM_SPN_SHIFT;

	/* Unanswered, a malformed intent expires in the firmware like any other. */
	if (ev->generation != im.generation || ev->relocator_id >= IM_MAX_RELOCATORS ||
	    ev->slot_idx >= IM_MAX_SLOTS || ev->start_spn % IM_SPNS_PER_GROUP ||
	    offset > im.hpa_size - IM_GROUP_BYTES) {
		atomic64_inc(&im.stale);
		return false;
	}
	if (im.replied_seq[ev->relocator_id][ev->slot_idx] == ev->seq) {
		atomic64_inc(&im.dup);
		return false;
	}
	return true;
}

static void im_reply(const struct im_event *ev, bool ack, ktime_t polled_at)
{
	u64 value = ev->relocator_id | ((u64)ev->slot_idx << 8) | ((u64)ack << 16) |
		    ((u64)ev->seq << 32);
	int ret;

	ret = im_hio(IO_OPCODE_SEND, IM_SUB_EVICT_REPLY, value, 0);
	if (ret == -EINVAL) {
		/* The firmware settled it first: it expired or a host command withdrew it. */
		atomic64_inc(&im.reply_settled);
		pr_info_ratelimited("im_evict: r%u/s%u seq %u settled by the firmware %lld ms after the poll\n",
				    ev->relocator_id, ev->slot_idx, ev->seq,
				    ktime_ms_delta(ktime_get(), polled_at));
	} else if (ret < 0) {
		atomic64_inc(&im.reply_err);
	} else {
		im.replied_seq[ev->relocator_id][ev->slot_idx] = ev->seq;
	}
}

static enum im_evict_verdict im_decide(const struct im_event *ev)
{
	u64 hpa = im.hpa_base + ((u64)ev->start_spn << IM_SPN_SHIFT);
	enum im_evict_verdict verdict = im_soft_fault_evict_intent(im.prov, PHYS_PFN(hpa));

	switch (verdict) {
	case IM_EVICT_PARKED:
		atomic64_inc(&im.ack_parked);
		break;
	case IM_EVICT_ALREADY_PARKED:
		atomic64_inc(&im.ack_already);
		break;
	case IM_EVICT_UNTRACKED:
		atomic64_inc(&im.ack_untracked);
		break;
	case IM_EVICT_REFUSED:
		atomic64_inc(&im.nack);
		break;
	}
	return verdict;
}

/*
 * Answers that need no cache writeback go out first, so a slow writeback does not hold back
 * the rest of the batch; a parked folio is ACKed only after its writeback, and so are its groups.
 */
static void im_handle_batch(const struct im_event *events, u32 count, ktime_t polled_at)
{
	u8 *verdicts;
	u64 hpa, t0;
	u32 i;

	verdicts = kcalloc(count, sizeof(*verdicts), GFP_KERNEL);
	if (!verdicts)
		return;
	for (i = 0; i < count; i++) {
		verdicts[i] = U8_MAX;
		if (!im_event_valid(&events[i]))
			continue;
		verdicts[i] = im_decide(&events[i]);
		if (verdicts[i] == IM_EVICT_UNTRACKED || verdicts[i] == IM_EVICT_REFUSED)
			im_reply(&events[i], verdicts[i] == IM_EVICT_UNTRACKED, polled_at);
	}
	for (i = 0; i < count; i++) {
		if (verdicts[i] != IM_EVICT_PARKED)
			continue;
		/* The device must see every dirty line before it may drop its copy. */
		hpa = im.hpa_base + ((u64)events[i].start_spn << IM_SPN_SHIFT);
		t0 = ktime_get_ns();
		im_writeback(ALIGN_DOWN(hpa, PMD_SIZE), PMD_SIZE);
		im_stat_time(&im.wb_ns_max, &im.wb_ns_total, ktime_get_ns() - t0);
		im_reply(&events[i], true, polled_at);
	}
	for (i = 0; i < count; i++) {
		if (verdicts[i] == IM_EVICT_ALREADY_PARKED)
			im_reply(&events[i], true, polled_at);
	}
	kfree(verdicts);
}

/* Caller holds im.lock. Returns the number of intents handled, or -errno. */
static int im_poll_once(void)
{
	const struct im_events_header *hdr = im.buf + IM_HIO_CMD_PAGE;
	struct im_event *events;
	ktime_t polled_at;
	u32 count;
	u64 t0;
	int ret;

	polled_at = ktime_get();
	t0 = ktime_get_ns();
	ret = im_hio(IO_OPCODE_RECV, IM_SUB_GET_EVICT_EVENTS, 0, IM_HIO_PAYLOAD);
	im_stat_time(&im.poll_ns_max, &im.poll_ns_total, ktime_get_ns() - t0);
	atomic64_inc(&im.polls);
	if (ret < 0) {
		atomic64_inc(&im.poll_err);
		return ret;
	}
	if (ret < sizeof(*hdr) || hdr->version != IM_EVENTS_VERSION) {
		atomic64_inc(&im.poll_err);
		return -EPROTO;
	}

	count = min_t(u32, hdr->count, (ret - sizeof(*hdr)) / sizeof(struct im_event));
	if (!count)
		return 0;

	/* Replies reuse the HIO buffer, so take the batch out first. */
	events = kmemdup((void *)(hdr + 1), count * sizeof(*events), GFP_KERNEL);
	if (!events)
		return -ENOMEM;
	atomic64_add(count, &im.events);
	im_handle_batch(events, count, polled_at);
	kfree(events);
	return count;
}

static int im_poll_thread(void *arg)
{
	while (!kthread_should_stop()) {
		int handled;

		mutex_lock(&im.lock);
		handled = im.stopping ? 0 : im_poll_once();
		mutex_unlock(&im.lock);
		if (handled <= 0)
			msleep_interruptible(READ_ONCE(im_poll_ms));
	}
	return 0;
}

static int im_fetch(void *priv, unsigned long pfn, unsigned long nr_pages)
{
	u64 hpa = PFN_PHYS(pfn);
	u64 offset = hpa - im.hpa_base;
	u64 len = (u64)nr_pages << PAGE_SHIFT;
	u64 host_addr = 0;
	u8 status = 0;
	long ret;
	u64 t0;

	if (hpa < im.hpa_base || offset >= im.hpa_size || len > im.hpa_size - offset)
		return -EINVAL;

	t0 = ktime_get_ns();
	ret = mx_kxfer_passthru(im.mx_pdev, IM_PREFETCH_SYNC, offset, len, false, &status,
				&host_addr);
	im_stat_time(&im.fetch_ns_max, &im.fetch_ns_total, ktime_get_ns() - t0);
	atomic64_inc(&im.fetches);
	if (!ret && status)
		ret = -EIO;
	if (ret)
		atomic64_inc(&im.fetch_err);
	return ret;
}

static const struct im_soft_fault_ops im_ops = {
	.fetch = im_fetch,
};

static int im_enable(void)
{
	int ret;

	mutex_lock(&im.lock);
	if (im.enabled) {
		/* A disable in progress still holds the provider and the HIO buffer. */
		ret = im.stopping ? -EBUSY : 0;
		goto unlock;
	}
	ret = -ENODEV;
	im.mx_pdev = mx_pdev_find(IM_DEV_ID);
	if (!im.mx_pdev)
		goto unlock;
	/* The kernel checks that the range is online memory of one node. */
	im.hpa_base = im.hpa_base_param;
	im.hpa_size = im.hpa_size_param;
	im.prov = im_soft_fault_register(&im_ops, NULL, PHYS_PFN(im.hpa_base),
					 PHYS_PFN(im.hpa_size));
	if (IS_ERR(im.prov)) {
		ret = PTR_ERR(im.prov);
		im.prov = NULL;
		goto unlock;
	}

	ret = -ENOMEM;
	im.buf = alloc_pages_exact(IM_HIO_BUF_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!im.buf)
		goto unregister;
	if (dmi_match(DMI_SYS_VENDOR, "QEMU"))
		im.cache_backdoor = ioremap(IM_QEMU_CACHE_BACKDOOR, PAGE_SIZE);

	im.stopping = false;
	im.generation = im.generation == U8_MAX ? 1 : im.generation + 1;
	memset(im.replied_seq, 0, sizeof(im.replied_seq));
	ret = im_hio(IO_OPCODE_SEND, IM_SUB_SET_EVICT_NOTIFY,
		     1 | ((u64)im.generation << 8) | ((u64)READ_ONCE(im_ack_timeout_ms) << 16), 0);
	if (ret < 0)
		goto free;

	im.poller = kthread_run(im_poll_thread, NULL, "mx_im_poll");
	if (IS_ERR(im.poller)) {
		ret = PTR_ERR(im.poller);
		im.poller = NULL;
		im_hio(IO_OPCODE_SEND, IM_SUB_SET_EVICT_NOTIFY, (u64)im.generation << 8, 0);
		goto free;
	}
	im.enabled = true;
	mutex_unlock(&im.lock);
	pr_info("im_evict: enabled (generation %u, hpa %#llx+%#llx, cache backdoor %s)\n",
		im.generation, im.hpa_base, im.hpa_size, im.cache_backdoor ? "on" : "off");
	return 0;

free:
	if (im.cache_backdoor)
		iounmap(im.cache_backdoor);
	im.cache_backdoor = NULL;
	im_hio_free_buf();
unregister:
	im_soft_fault_unregister(im.prov);
	im.prov = NULL;
unlock:
	mutex_unlock(&im.lock);
	return ret;
}

static void im_disable(void)
{
	struct task_struct *poller;

	mutex_lock(&im.lock);
	/* One disable tears down; a second would stop a NULL poller and free the provider twice. */
	if (!im.enabled || im.stopping) {
		mutex_unlock(&im.lock);
		return;
	}
	poller = im.poller;
	im.poller = NULL;
	im.stopping = true;
	mutex_unlock(&im.lock);

	/* The poller takes im.lock per pass, so it must be stopped without holding it. */
	kthread_stop(poller);

	mutex_lock(&im.lock);
	if (im_hio(IO_OPCODE_SEND, IM_SUB_SET_EVICT_NOTIFY, (u64)im.generation << 8, 0) < 0)
		pr_warn("im_evict: SetEvictNotify(disable) failed\n");
	mutex_unlock(&im.lock);

	/* Pages every folio still parked back in through im_fetch() before it returns. */
	im_soft_fault_unregister(im.prov);

	mutex_lock(&im.lock);
	im.prov = NULL;
	if (im.cache_backdoor)
		iounmap(im.cache_backdoor);
	im.cache_backdoor = NULL;
	im_hio_free_buf();
	im.enabled = false;
	mutex_unlock(&im.lock);
	pr_info("im_evict: disabled\n");
}

static ssize_t im_enable_write(struct file *file, const char __user *buf, size_t count,
			       loff_t *ppos)
{
	bool enable;
	int ret;

	ret = kstrtobool_from_user(buf, count, &enable);
	if (ret)
		return ret;
	if (enable) {
		ret = im_enable();
		if (ret)
			return ret;
	} else {
		im_disable();
	}
	return count;
}

static const struct file_operations im_enable_fops = {
	.write = im_enable_write,
	.llseek = noop_llseek,
};

static int im_stats_show(struct seq_file *m, void *v)
{
	u64 polls = atomic64_read(&im.polls), fetches = atomic64_read(&im.fetches);
	u64 parked = atomic64_read(&im.ack_parked);

	seq_printf(m, "enabled=%d generation=%u hpa_base=%#llx hpa_size=%#llx", im.enabled,
		   im.generation, im.hpa_base, im.hpa_size);
	seq_printf(m, " poll_ms=%u ack_timeout_ms=%u\n", READ_ONCE(im_poll_ms),
		   READ_ONCE(im_ack_timeout_ms));
	seq_printf(m, "polls=%lld poll_err=%lld events=%lld stale=%lld dup=%lld\n", polls,
		   atomic64_read(&im.poll_err), atomic64_read(&im.events), atomic64_read(&im.stale),
		   atomic64_read(&im.dup));
	seq_printf(m, "ack_untracked=%lld ack_parked=%lld ack_already=%lld nack=%lld",
		   atomic64_read(&im.ack_untracked), atomic64_read(&im.ack_parked),
		   atomic64_read(&im.ack_already), atomic64_read(&im.nack));
	seq_printf(m, " reply_settled=%lld reply_err=%lld\n", atomic64_read(&im.reply_settled),
		   atomic64_read(&im.reply_err));
	seq_printf(m, "poll_us_avg=%lld poll_us_max=%lld wb_us_avg=%lld wb_us_max=%lld\n",
		   polls ? atomic64_read(&im.poll_ns_total) / polls / 1000 : 0,
		   atomic64_read(&im.poll_ns_max) / 1000,
		   parked ? atomic64_read(&im.wb_ns_total) / parked / 1000 : 0,
		   atomic64_read(&im.wb_ns_max) / 1000);
	seq_printf(m, "fetches=%lld fetch_err=%lld fetch_us_avg=%lld fetch_us_max=%lld\n", fetches,
		   atomic64_read(&im.fetch_err),
		   fetches ? atomic64_read(&im.fetch_ns_total) / fetches / 1000 : 0,
		   atomic64_read(&im.fetch_ns_max) / 1000);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(im_stats);

void mx_im_init(void)
{
	im.dir = debugfs_create_dir("mx_dma_im", NULL);
	debugfs_create_x64("hpa_base", 0600, im.dir, &im.hpa_base_param);
	debugfs_create_x64("hpa_size", 0600, im.dir, &im.hpa_size_param);
	debugfs_create_file("enable", 0200, im.dir, NULL, &im_enable_fops);
	debugfs_create_file("stats", 0400, im.dir, NULL, &im_stats_fops);
}

void mx_im_exit(void)
{
	im_disable();
	debugfs_remove_recursive(im.dir);
	im.dir = NULL;
}

#endif /* CONFIG_IM_SOFT_FAULT */

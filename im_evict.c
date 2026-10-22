// SPDX-License-Identifier: GPL-2.0
/*
 * Infinite Memory eviction notify (PoC), one instance per device.
 * Eviction intents polled over HIO are answered after the kernel parks the host folio;
 * a fault on a parked folio pages it back in with PrefetchSync.
 */
#include "mx_dma.h"

#if IS_ENABLED(CONFIG_IM_SOFT_FAULT)

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/im_soft_fault.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/namei.h>
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
#define IM_MAX_RELOCATORS	64
#define IM_MAX_SLOTS		16
#define IM_ACK_TIMEOUT_DEFAULT_MS	5000
#define IM_ACK_TIMEOUT_MAX_MS	(60 * 60 * 1000)

#define IM_CXL_DEVICES		"/sys/bus/cxl/devices"
#define IM_SCAN_MAX		64
#define IM_NAME_LEN		32

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

static bool im_cache_backdoor;
module_param(im_cache_backdoor, bool, 0644);
MODULE_PARM_DESC(im_cache_backdoor, "Write back CPU caches through the QEMU host-cache model (emulator only)");

struct mx_im {
	struct mx_pci_dev *mx_pdev;
	/* Held across a whole enable, disable or detach; none returns while another is midway. */
	struct mutex ctl_lock;
	/* Serializes the poller with control steps and every use of the shared HIO buffer. */
	struct mutex lock;
	struct im_soft_fault_provider *prov;
	struct task_struct *poller;
	void *buf;
	void __iomem *cache_backdoor;
	/* Where the device's memory is mapped: HPA = region_base + (DPA - dpa_base). */
	u64 region_base, region_size, dpa_base;
	/* The part of the region online as system RAM; only its folios can be parked. */
	u64 ram_base, ram_size;
	u64 tag;
	unsigned int ack_timeout_ms;
	u8 generation;
	bool enabled;
	/* Set under lock before kthread_stop(), whose wakeup would interrupt an HIO wait. */
	bool stopping;
	/* Set when the device is going away; no enable is accepted after it. */
	bool dead;
	struct work_struct reset_work;
	struct dentry *dir;
	/* The firmware applies a verdict asynchronously, so an answered intent can be listed again. */
	u32 replied_seq[IM_MAX_RELOCATORS][IM_MAX_SLOTS];

	atomic64_t polls, poll_err, events, stale, dup, ack_untracked, ack_outside, ack_parked,
		ack_already, nack, reply_settled, reply_err, fetches, fetch_err;
	atomic64_t poll_ns_max, poll_ns_total, fetch_ns_max, fetch_ns_total, wb_ns_max, wb_ns_total;
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
static void im_hio_abandon_buf(struct mx_im *im)
{
	pr_warn("im_evict: HIO transfer did not complete, abandoning its buffer\n");
	im->buf = alloc_pages_exact(IM_HIO_BUF_SIZE, GFP_KERNEL | __GFP_ZERO);
}

static void im_hio_free_buf(struct mx_im *im)
{
	if (im->buf)
		free_pages_exact(im->buf, IM_HIO_BUF_SIZE);
	im->buf = NULL;
}

/* Returns the payload size the firmware wrote, or -errno. */
static int im_hio(struct mx_im *im, int opcode, u32 sub_opcode, u64 value, u32 payload_size)
{
	struct im_hio_header *hdr = im->buf;
	struct im_gaia_payload *payload = im->buf + sizeof(*hdr);
	ssize_t ret;

	lockdep_assert_held(&im->lock);
	if (!im->buf)
		return -ENOMEM;
	memset(im->buf, 0, IM_HIO_CMD_PAGE);
	hdr->protocol_opcode = IM_HIO_GAIA_COMMAND;
	hdr->data_size = payload_size;
	hdr->tag = ++im->tag;
	payload->magic = IM_GAIA_MAGIC;
	payload->version = IM_GAIA_VERSION;
	payload->gaia_opcode = IM_GAIA_SYSTEM_COMMAND;
	payload->arg1 = sub_opcode;
	payload->arg2 = value;

	ret = mx_kxfer_protocol(im->mx_pdev, im->buf, IM_HIO_CMD_PAGE + payload_size, opcode);
	if (ret == -ETIMEDOUT || ret == -EINTR || ret == -EIO)
		im_hio_abandon_buf(im);
	if (ret < 0)
		return ret;
	if (hdr->tag != im->tag)
		return -EPROTO;
	ret = im_hio_status_to_errno(hdr->status);
	if (ret)
		return ret;
	return min_t(u32, hdr->data_size, payload_size);
}

static void im_writeback(struct mx_im *im, u64 hpa, u64 len)
{
	u64 addr;

	if (im->cache_backdoor) {
		for (addr = hpa; addr < hpa + len; addr += IM_CACHE_LINE)
			writeq(addr, im->cache_backdoor);
		return;
	}
#ifdef CONFIG_X86
	clflush_cache_range(phys_to_virt(hpa), len);
#endif
}

/* Maps an intent to its HPA, or returns false when it lies outside the device's region. */
static bool im_event_hpa(const struct mx_im *im, const struct im_event *ev, u64 *hpa)
{
	u64 dpa = (u64)ev->start_spn << IM_SPN_SHIFT;

	if (ev->start_spn % IM_SPNS_PER_GROUP || dpa < im->dpa_base ||
	    dpa - im->dpa_base > im->region_size - IM_GROUP_BYTES)
		return false;
	*hpa = im->region_base + (dpa - im->dpa_base);
	return true;
}

static bool im_event_valid(struct mx_im *im, const struct im_event *ev)
{
	u64 hpa;

	/* Unanswered, a malformed intent expires in the firmware like any other. */
	if (ev->generation != im->generation || ev->relocator_id >= IM_MAX_RELOCATORS ||
	    ev->slot_idx >= IM_MAX_SLOTS || !im_event_hpa(im, ev, &hpa)) {
		atomic64_inc(&im->stale);
		return false;
	}
	if (im->replied_seq[ev->relocator_id][ev->slot_idx] == ev->seq) {
		atomic64_inc(&im->dup);
		return false;
	}
	return true;
}

static void im_reply(struct mx_im *im, const struct im_event *ev, bool ack, ktime_t polled_at)
{
	u64 value = ev->relocator_id | ((u64)ev->slot_idx << 8) | ((u64)ack << 16) |
		    ((u64)ev->seq << 32);
	int ret;

	ret = im_hio(im, IO_OPCODE_SEND, IM_SUB_EVICT_REPLY, value, 0);
	if (ret == -EINVAL) {
		/* The firmware settled it first: it expired or a host command withdrew it. */
		atomic64_inc(&im->reply_settled);
		pr_info_ratelimited("im_evict: r%u/s%u seq %u settled by the firmware %lld ms after the poll\n",
				    ev->relocator_id, ev->slot_idx, ev->seq,
				    ktime_ms_delta(ktime_get(), polled_at));
	} else if (ret < 0) {
		atomic64_inc(&im->reply_err);
	} else {
		im->replied_seq[ev->relocator_id][ev->slot_idx] = ev->seq;
	}
}

static enum im_evict_verdict im_decide(struct mx_im *im, const struct im_event *ev, u64 hpa)
{
	enum im_evict_verdict verdict;

	/* Device memory that is not system RAM here (devdax) has no host folio to park. */
	if (hpa < im->ram_base || hpa - im->ram_base >= im->ram_size) {
		atomic64_inc(&im->ack_outside);
		return IM_EVICT_UNTRACKED;
	}
	verdict = im_soft_fault_evict_intent(im->prov, PHYS_PFN(hpa));
	switch (verdict) {
	case IM_EVICT_PARKED:
		atomic64_inc(&im->ack_parked);
		break;
	case IM_EVICT_ALREADY_PARKED:
		atomic64_inc(&im->ack_already);
		break;
	case IM_EVICT_UNTRACKED:
		atomic64_inc(&im->ack_untracked);
		break;
	case IM_EVICT_REFUSED:
		atomic64_inc(&im->nack);
		break;
	}
	return verdict;
}

/*
 * Answers that need no cache writeback go out first, so a slow writeback does not hold back
 * the rest of the batch; a parked folio is ACKed only after its writeback, and so are its groups.
 */
static void im_handle_batch(struct mx_im *im, const struct im_event *events, u32 count,
			    ktime_t polled_at)
{
	u8 *verdicts;
	u64 hpa, t0;
	u32 i;

	verdicts = kcalloc(count, sizeof(*verdicts), GFP_KERNEL);
	if (!verdicts)
		return;
	for (i = 0; i < count; i++) {
		verdicts[i] = U8_MAX;
		if (!im_event_valid(im, &events[i]))
			continue;
		im_event_hpa(im, &events[i], &hpa);
		verdicts[i] = im_decide(im, &events[i], hpa);
		if (verdicts[i] == IM_EVICT_UNTRACKED || verdicts[i] == IM_EVICT_REFUSED)
			im_reply(im, &events[i], verdicts[i] == IM_EVICT_UNTRACKED, polled_at);
	}
	for (i = 0; i < count; i++) {
		if (verdicts[i] != IM_EVICT_PARKED)
			continue;
		/* The device must see every dirty line before it may drop its copy. */
		im_event_hpa(im, &events[i], &hpa);
		t0 = ktime_get_ns();
		im_writeback(im, ALIGN_DOWN(hpa, PMD_SIZE), PMD_SIZE);
		im_stat_time(&im->wb_ns_max, &im->wb_ns_total, ktime_get_ns() - t0);
		im_reply(im, &events[i], true, polled_at);
	}
	for (i = 0; i < count; i++) {
		if (verdicts[i] == IM_EVICT_ALREADY_PARKED)
			im_reply(im, &events[i], true, polled_at);
	}
	kfree(verdicts);
}

/* Returns the number of intents handled, or -errno. */
static int im_poll_once(struct mx_im *im)
{
	const struct im_events_header *hdr = im->buf + IM_HIO_CMD_PAGE;
	struct im_event *events;
	ktime_t polled_at;
	u32 count;
	u64 t0;
	int ret;

	polled_at = ktime_get();
	t0 = ktime_get_ns();
	ret = im_hio(im, IO_OPCODE_RECV, IM_SUB_GET_EVICT_EVENTS, 0, IM_HIO_PAYLOAD);
	im_stat_time(&im->poll_ns_max, &im->poll_ns_total, ktime_get_ns() - t0);
	atomic64_inc(&im->polls);
	if (ret < 0) {
		atomic64_inc(&im->poll_err);
		return ret;
	}
	if (ret < sizeof(*hdr) || hdr->version != IM_EVENTS_VERSION) {
		atomic64_inc(&im->poll_err);
		return -EPROTO;
	}

	count = min_t(u32, hdr->count, (ret - sizeof(*hdr)) / sizeof(struct im_event));
	if (!count)
		return 0;

	/* Replies reuse the HIO buffer, so take the batch out first. */
	events = kmemdup((void *)(hdr + 1), count * sizeof(*events), GFP_KERNEL);
	if (!events)
		return -ENOMEM;
	atomic64_add(count, &im->events);
	im_handle_batch(im, events, count, polled_at);
	kfree(events);
	return count;
}

static int im_poll_thread(void *arg)
{
	struct mx_im *im = arg;

	while (!kthread_should_stop()) {
		int handled;

		mutex_lock(&im->lock);
		handled = im->stopping ? 0 : im_poll_once(im);
		mutex_unlock(&im->lock);
		if (handled <= 0)
			msleep_interruptible(READ_ONCE(im_poll_ms));
	}
	return 0;
}

static int im_fetch(void *priv, unsigned long pfn, unsigned long nr_pages)
{
	struct mx_im *im = priv;
	u64 hpa = PFN_PHYS(pfn);
	u64 len = (u64)nr_pages << PAGE_SHIFT;
	u64 host_addr = 0;
	u8 status = 0;
	long ret;
	u64 t0;

	if (hpa < im->ram_base || hpa - im->ram_base >= im->ram_size ||
	    len > im->ram_size - (hpa - im->ram_base))
		return -EINVAL;

	t0 = ktime_get_ns();
	ret = mx_kxfer_passthru(im->mx_pdev, IM_PREFETCH_SYNC,
				hpa - im->region_base + im->dpa_base, len, false, &status,
				&host_addr);
	im_stat_time(&im->fetch_ns_max, &im->fetch_ns_total, ktime_get_ns() - t0);
	atomic64_inc(&im->fetches);
	if (!ret && status)
		ret = -EIO;
	if (ret)
		atomic64_inc(&im->fetch_err);
	return ret;
}

static const struct im_soft_fault_ops im_ops = {
	.fetch = im_fetch,
};

static int im_read_sysfs(const char *path, char *buf, size_t len)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_read(f, buf, len - 1, &pos);
	filp_close(f, NULL);
	if (n < 0)
		return n;
	buf[n] = '\0';
	strim(buf);
	return 0;
}

static int im_read_u64(const char *path, u64 *val)
{
	char buf[32];
	int ret = im_read_sysfs(path, buf, sizeof(buf));

	return ret ? ret : kstrtou64(buf, 0, val);
}

/* Name of what @path resolves to, e.g. the device a sysfs link points at. */
static int im_link_name(const char *path, char *name, size_t len)
{
	struct path p;
	int ret = kern_path(path, LOOKUP_FOLLOW, &p);

	if (ret)
		return ret;
	strscpy(name, p.dentry->d_name.name, len);
	path_put(&p);
	return 0;
}

static bool im_path_exists(const char *path)
{
	struct path p;

	if (kern_path(path, 0, &p))
		return false;
	path_put(&p);
	return true;
}

struct im_dir_scan {
	struct dir_context ctx;
	const char *prefix;
	int count;
	char names[IM_SCAN_MAX][IM_NAME_LEN];
};

static bool im_dir_actor(struct dir_context *ctx, const char *name, int len, loff_t off,
			 u64 ino, unsigned int type)
{
	struct im_dir_scan *s = container_of(ctx, struct im_dir_scan, ctx);
	size_t plen = strlen(s->prefix);

	if (len < plen || len >= IM_NAME_LEN || strncmp(name, s->prefix, plen))
		return true;
	if (s->count == IM_SCAN_MAX)
		return false;
	memcpy(s->names[s->count], name, len);
	s->names[s->count][len] = '\0';
	s->count++;
	return true;
}

static int im_dir_scan(struct im_dir_scan *s, const char *dir, const char *prefix)
{
	struct file *f;
	int ret;

	memset(s, 0, sizeof(*s));
	s->ctx.actor = im_dir_actor;
	s->prefix = prefix;
	f = filp_open(dir, O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	ret = iterate_dir(f, &s->ctx);
	filp_close(f, NULL);
	return ret;
}

/* Takes the one dax device of @region bound to kmem as the system-RAM range. */
static int im_find_ram(struct mx_im *im, const char *region, struct im_dir_scan *s, char *path,
		       size_t plen)
{
	char name[IM_NAME_LEN];
	u64 base, size;
	int i, found = 0;

	snprintf(path, plen, IM_CXL_DEVICES "/%s/dax_%s", region, region);
	if (im_dir_scan(s, path, "dax"))
		return -ENODEV;
	for (i = 0; i < s->count; i++) {
		snprintf(path, plen, IM_CXL_DEVICES "/%s/dax_%s/%s/driver", region, region,
			 s->names[i]);
		if (im_link_name(path, name, sizeof(name)) || strcmp(name, "kmem"))
			continue;
		snprintf(path, plen, IM_CXL_DEVICES "/%s/dax_%s/%s/resource", region, region,
			 s->names[i]);
		if (im_read_u64(path, &base))
			return -ENODEV;
		snprintf(path, plen, IM_CXL_DEVICES "/%s/dax_%s/%s/size", region, region,
			 s->names[i]);
		if (im_read_u64(path, &size))
			return -ENODEV;
		/* For a device built from several ranges, base + size covers others' memory. */
		snprintf(path, plen, IM_CXL_DEVICES "/%s/dax_%s/%s/mapping1", region, region,
			 s->names[i]);
		if (found++ || im_path_exists(path))
			return -EOPNOTSUPP;
		im->ram_base = base;
		im->ram_size = size;
	}
	return found ? 0 : -ENODEV;
}

/*
 * Finds the region the device is mapped through, by the rule pxl's coordinator uses:
 * the region whose target0 decoder sits on a port whose uport is this device's memdev.
 */
static int im_find_range(struct mx_im *im)
{
	char memdev[IM_NAME_LEN], name[IM_NAME_LEN], decoder[IM_NAME_LEN], path[192];
	struct im_dir_scan *regions, *scan;
	u64 ways;
	int i, ret;

	regions = kmalloc(sizeof(*regions), GFP_KERNEL);
	scan = kmalloc(sizeof(*scan), GFP_KERNEL);
	ret = -ENOMEM;
	if (!regions || !scan)
		goto out;
	snprintf(memdev, sizeof(memdev), "mem%d", im->mx_pdev->dev_id);
	ret = im_dir_scan(regions, IM_CXL_DEVICES, "region");
	if (ret)
		goto out;

	ret = -ENODEV;
	for (i = 0; i < regions->count; i++) {
		const char *r = regions->names[i];

		snprintf(path, sizeof(path), IM_CXL_DEVICES "/%s/target0", r);
		if (im_read_sysfs(path, decoder, sizeof(decoder)) || !decoder[0])
			continue;
		snprintf(path, sizeof(path), IM_CXL_DEVICES "/%s/../uport", decoder);
		if (im_link_name(path, name, sizeof(name)) || strcmp(name, memdev))
			continue;

		snprintf(path, sizeof(path), IM_CXL_DEVICES "/%s/interleave_ways", r);
		if (im_read_u64(path, &ways))
			break;
		/* With interleave, the device's DPAs are not one linear run of HPAs. */
		if (ways != 1) {
			ret = -EOPNOTSUPP;
			break;
		}
		snprintf(path, sizeof(path), IM_CXL_DEVICES "/%s/resource", r);
		if (im_read_u64(path, &im->region_base))
			break;
		snprintf(path, sizeof(path), IM_CXL_DEVICES "/%s/size", r);
		if (im_read_u64(path, &im->region_size))
			break;
		snprintf(path, sizeof(path), IM_CXL_DEVICES "/%s/dpa_resource", decoder);
		if (im_read_u64(path, &im->dpa_base))
			break;
		ret = im_find_ram(im, r, scan, path, sizeof(path));
		if (!ret && (im->ram_base < im->region_base ||
			     im->ram_base + im->ram_size > im->region_base + im->region_size))
			ret = -ERANGE;
		break;
	}
out:
	kfree(scan);
	kfree(regions);
	return ret;
}

static int im_enable(struct mx_im *im)
{
	int ret;

	mutex_lock(&im->ctl_lock);
	mutex_lock(&im->lock);
	ret = -ENODEV;
	if (im->dead)
		goto unlock;
	ret = 0;
	if (im->enabled)
		goto unlock;
	ret = im_find_range(im);
	if (ret) {
		pr_warn("im_evict: mx_dma%d has no usable IM range (%d)\n", im->mx_pdev->dev_id,
			ret);
		goto unlock;
	}
	/* The kernel checks that the range is online memory of one node. */
	im->prov = im_soft_fault_register(&im_ops, im, PHYS_PFN(im->ram_base),
					  PHYS_PFN(im->ram_size));
	if (IS_ERR(im->prov)) {
		ret = PTR_ERR(im->prov);
		im->prov = NULL;
		goto unlock;
	}

	ret = -ENOMEM;
	im->buf = alloc_pages_exact(IM_HIO_BUF_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!im->buf)
		goto unregister;
	if (READ_ONCE(im_cache_backdoor)) {
		im->cache_backdoor = ioremap(IM_QEMU_CACHE_BACKDOOR, PAGE_SIZE);
		if (!im->cache_backdoor)
			goto free;
	}

	im->stopping = false;
	im->generation = im->generation == U8_MAX ? 1 : im->generation + 1;
	memset(im->replied_seq, 0, sizeof(im->replied_seq));
	ret = im_hio(im, IO_OPCODE_SEND, IM_SUB_SET_EVICT_NOTIFY,
		     1 | ((u64)im->generation << 8) | ((u64)im->ack_timeout_ms << 16), 0);
	if (ret < 0)
		goto free;

	im->poller = kthread_run(im_poll_thread, im, "mx_im_poll%d", im->mx_pdev->dev_id);
	if (IS_ERR(im->poller)) {
		ret = PTR_ERR(im->poller);
		im->poller = NULL;
		im_hio(im, IO_OPCODE_SEND, IM_SUB_SET_EVICT_NOTIFY, (u64)im->generation << 8, 0);
		goto free;
	}
	im->enabled = true;
	mutex_unlock(&im->lock);
	pr_info("im_evict: mx_dma%d enabled (generation %u, ram %#llx+%#llx, cache backdoor %s)\n",
		im->mx_pdev->dev_id, im->generation, im->ram_base, im->ram_size,
		im->cache_backdoor ? "on" : "off");
	mutex_unlock(&im->ctl_lock);
	return 0;

free:
	if (im->cache_backdoor)
		iounmap(im->cache_backdoor);
	im->cache_backdoor = NULL;
	im_hio_free_buf(im);
unregister:
	im_soft_fault_unregister(im->prov);
	im->prov = NULL;
unlock:
	mutex_unlock(&im->lock);
	mutex_unlock(&im->ctl_lock);
	return ret;
}

static void im_disable_locked(struct mx_im *im)
{
	struct task_struct *poller;

	lockdep_assert_held(&im->ctl_lock);
	mutex_lock(&im->lock);
	if (!im->enabled) {
		mutex_unlock(&im->lock);
		return;
	}
	poller = im->poller;
	im->poller = NULL;
	im->stopping = true;
	mutex_unlock(&im->lock);

	/* The poller takes im->lock per pass, so it must be stopped without holding it. */
	kthread_stop(poller);

	mutex_lock(&im->lock);
	if (im_hio(im, IO_OPCODE_SEND, IM_SUB_SET_EVICT_NOTIFY, (u64)im->generation << 8, 0) < 0)
		pr_warn("im_evict: mx_dma%d SetEvictNotify(disable) failed\n", im->mx_pdev->dev_id);
	mutex_unlock(&im->lock);

	/* Pages every folio still parked back in through im_fetch() before it returns. */
	im_soft_fault_unregister(im->prov);

	mutex_lock(&im->lock);
	im->prov = NULL;
	if (im->cache_backdoor)
		iounmap(im->cache_backdoor);
	im->cache_backdoor = NULL;
	im_hio_free_buf(im);
	im->enabled = false;
	mutex_unlock(&im->lock);
	pr_info("im_evict: mx_dma%d disabled\n", im->mx_pdev->dev_id);
}

static void im_disable(struct mx_im *im)
{
	mutex_lock(&im->ctl_lock);
	im_disable_locked(im);
	mutex_unlock(&im->ctl_lock);
}

static struct mx_im *im_of(struct device *dev)
{
	struct mx_pci_dev *mx_pdev = dev_get_drvdata(dev);

	return mx_pdev ? READ_ONCE(mx_pdev->im) : NULL;
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mx_im *im = im_of(dev);

	if (!im)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", READ_ONCE(im->enabled));
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct mx_im *im = im_of(dev);
	bool enable;
	int ret;

	if (!im)
		return -ENODEV;
	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
	if (enable) {
		ret = im_enable(im);
		if (ret)
			return ret;
	} else {
		im_disable(im);
	}
	return count;
}
static DEVICE_ATTR_RW(enable);

static ssize_t ack_timeout_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mx_im *im = im_of(dev);

	if (!im)
		return -ENODEV;
	return sysfs_emit(buf, "%u\n", READ_ONCE(im->ack_timeout_ms));
}

/* Applies from the next enable: the firmware takes it with SetEvictNotify. */
static ssize_t ack_timeout_ms_store(struct device *dev, struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct mx_im *im = im_of(dev);
	unsigned int ms;
	int ret;

	if (!im)
		return -ENODEV;
	ret = kstrtouint(buf, 0, &ms);
	if (ret)
		return ret;
	if (!ms || ms > IM_ACK_TIMEOUT_MAX_MS)
		return -EINVAL;
	WRITE_ONCE(im->ack_timeout_ms, ms);
	return count;
}
static DEVICE_ATTR_RW(ack_timeout_ms);

static struct attribute *mx_im_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_ack_timeout_ms.attr,
	NULL,
};

const struct attribute_group mx_im_group = {
	.name = "im",
	.attrs = mx_im_attrs,
};

static int im_stats_show(struct seq_file *m, void *v)
{
	struct mx_im *im = m->private;
	u64 polls = atomic64_read(&im->polls), fetches = atomic64_read(&im->fetches);
	u64 parked = atomic64_read(&im->ack_parked);

	seq_printf(m, "enabled=%d generation=%u ack_timeout_ms=%u poll_ms=%u\n", im->enabled,
		   im->generation, READ_ONCE(im->ack_timeout_ms), READ_ONCE(im_poll_ms));
	seq_printf(m, "region_base=%#llx region_size=%#llx dpa_base=%#llx", im->region_base,
		   im->region_size, im->dpa_base);
	seq_printf(m, " ram_base=%#llx ram_size=%#llx\n", im->ram_base, im->ram_size);
	seq_printf(m, "polls=%lld poll_err=%lld events=%lld stale=%lld dup=%lld\n", polls,
		   atomic64_read(&im->poll_err), atomic64_read(&im->events),
		   atomic64_read(&im->stale), atomic64_read(&im->dup));
	seq_printf(m, "ack_untracked=%lld ack_outside=%lld ack_parked=%lld ack_already=%lld nack=%lld",
		   atomic64_read(&im->ack_untracked), atomic64_read(&im->ack_outside), parked,
		   atomic64_read(&im->ack_already), atomic64_read(&im->nack));
	seq_printf(m, " reply_settled=%lld reply_err=%lld\n", atomic64_read(&im->reply_settled),
		   atomic64_read(&im->reply_err));
	seq_printf(m, "poll_us_avg=%lld poll_us_max=%lld wb_us_avg=%lld wb_us_max=%lld\n",
		   polls ? atomic64_read(&im->poll_ns_total) / polls / 1000 : 0,
		   atomic64_read(&im->poll_ns_max) / 1000,
		   parked ? atomic64_read(&im->wb_ns_total) / parked / 1000 : 0,
		   atomic64_read(&im->wb_ns_max) / 1000);
	seq_printf(m, "fetches=%lld fetch_err=%lld fetch_us_avg=%lld fetch_us_max=%lld\n", fetches,
		   atomic64_read(&im->fetch_err),
		   fetches ? atomic64_read(&im->fetch_ns_total) / fetches / 1000 : 0,
		   atomic64_read(&im->fetch_ns_max) / 1000);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(im_stats);

/*
 * A driver that went away while enabled leaves the firmware posting intents nobody answers.
 * Turning the notify off runs from a work item, so an attach never waits on firmware without IM.
 */
static void im_reset_work(struct work_struct *work)
{
	struct mx_im *im = container_of(work, struct mx_im, reset_work);
	int ret;

	mutex_lock(&im->lock);
	if (im->enabled || im->dead)
		goto unlock;
	im->buf = alloc_pages_exact(IM_HIO_BUF_SIZE, GFP_KERNEL | __GFP_ZERO);
	ret = im_hio(im, IO_OPCODE_SEND, IM_SUB_SET_EVICT_NOTIFY, 0, 0);
	im_hio_free_buf(im);
	pr_info("im_evict: mx_dma%d firmware eviction notify reset (%d)\n", im->mx_pdev->dev_id,
		ret < 0 ? ret : 0);
unlock:
	mutex_unlock(&im->lock);
}

void mx_im_attach(struct mx_pci_dev *mx_pdev)
{
	struct mx_im *im;
	char name[IM_NAME_LEN];

	im = kzalloc(sizeof(*im), GFP_KERNEL);
	if (!im)
		return;
	mutex_init(&im->ctl_lock);
	mutex_init(&im->lock);
	INIT_WORK(&im->reset_work, im_reset_work);
	im->mx_pdev = mx_pdev;
	im->ack_timeout_ms = IM_ACK_TIMEOUT_DEFAULT_MS;
	snprintf(name, sizeof(name), "mx_dma%d_im", mx_pdev->dev_id);
	im->dir = debugfs_create_dir(name, NULL);
	debugfs_create_file("stats", 0400, im->dir, im, &im_stats_fops);
	WRITE_ONCE(mx_pdev->im, im);
	schedule_work(&im->reset_work);
}

/* Stops IM while the device's transport still works; parked folios are paged back in. */
void mx_im_detach(struct mx_pci_dev *mx_pdev)
{
	struct mx_im *im = mx_pdev->im;

	if (!im)
		return;
	mutex_lock(&im->ctl_lock);
	mutex_lock(&im->lock);
	im->dead = true;
	mutex_unlock(&im->lock);
	cancel_work_sync(&im->reset_work);
	im_disable_locked(im);
	mutex_unlock(&im->ctl_lock);
	debugfs_remove_recursive(im->dir);
	im->dir = NULL;
}

/* Called once the sysfs files that reach the instance are gone. */
void mx_im_free(struct mx_pci_dev *mx_pdev)
{
	struct mx_im *im = mx_pdev->im;

	WRITE_ONCE(mx_pdev->im, NULL);
	kfree(im);
}

#endif /* CONFIG_IM_SOFT_FAULT */

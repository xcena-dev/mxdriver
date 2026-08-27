// SPDX-License-Identifier: GPL-2.0

#include <linux/file.h>
#if __has_include(<linux/filelock.h>)
#include <linux/filelock.h>
#endif
#include <linux/capability.h>
#include <linux/kdev_t.h>
#include <linux/magic.h>
#include <linux/random.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "mx_dma.h"

#define MX_LEASE_CAPABILITIES (MX_LEASE_CAP_OFD_LIFETIME | \
	MX_LEASE_CAP_STATE_INODE_ANCHOR | MX_LEASE_CAP_FAMILY_EXCLUSION | \
	MX_LEASE_CAP_ATOMIC_TRANSITION | MX_LEASE_CAP_HUB_REPLACEMENT | \
	MX_LEASE_CAP_LEGACY_EXCLUSION | \
	MX_LEASE_CAP_PRIVILEGED_REPLACEMENT | \
	MX_LEASE_CAP_RESERVED_HIO_QID | \
	MX_LEASE_CAP_PERSISTENT_STATE_ANCHOR | \
	MX_LEASE_CAP_PRIVILEGED_FRESH_ANCHOR | \
	MX_LEASE_CAP_PRIVILEGED_PUBLISHER | \
	MX_LEASE_CAP_WORKLOAD_PROOF_BINDING | \
	MX_LEASE_CAP_CANONICAL_SLOT_OFD_PROOF)

static_assert(sizeof(struct mx_lease_caps) == 64);
static_assert(sizeof(struct mx_lease_acquire) == 128);
static_assert(sizeof(struct mx_lease_transition) == 96);
static_assert(sizeof(struct mx_lease_query) == 128);
static_assert(sizeof(struct mx_lease_slot_domain) == 64);
static_assert(sizeof(struct mx_lease_proofs) == 128);

static void mx_pdev_free(struct kref *ref)
{
	struct mx_pci_dev *mx_pdev = container_of(ref, struct mx_pci_dev, refcount);
	struct path state_path = {};
	struct path slot_domain_path = {};
	bool state_path_valid;
	bool slot_domain_path_valid;

	mutex_lock(&mx_pdev->lease.lock);
	if (WARN_ON_ONCE(mx_pdev->lease.state.family_holders ||
			 mx_pdev->lease.state.publishers ||
			 mx_pdev->lease.state.workloads ||
			 mx_pdev->lease.state.legacy_holders)) {
		memset(&mx_pdev->lease.state, 0, sizeof(mx_pdev->lease.state));
	}
	state_path_valid = mx_pdev->lease.state_path_valid;
	if (state_path_valid)
		state_path = mx_pdev->lease.state_path;
	slot_domain_path_valid = mx_pdev->lease.slot_domain_path_valid;
	if (slot_domain_path_valid)
		slot_domain_path = mx_pdev->lease.slot_domain_path;
	mx_pdev->lease.state_path_valid = false;
	mx_pdev->lease.state_path.mnt = NULL;
	mx_pdev->lease.state_path.dentry = NULL;
	mx_pdev->lease.state_inode = NULL;
	mx_pdev->lease.slot_domain_path_valid = false;
	mx_pdev->lease.slot_domain_path.mnt = NULL;
	mx_pdev->lease.slot_domain_path.dentry = NULL;
	mx_pdev->lease.slot_domain_inode = NULL;
	mutex_unlock(&mx_pdev->lease.lock);
	if (state_path_valid)
		path_put(&state_path);
	if (slot_domain_path_valid)
		path_put(&slot_domain_path);
	mx_pdev->magic = 0;
	kfree(mx_pdev);
}

void mx_lease_init(struct mx_pci_dev *mx_pdev)
{
	u64 incarnation;

	kref_init(&mx_pdev->refcount);
	init_rwsem(&mx_pdev->io_rwsem);
	mutex_init(&mx_pdev->lease.lock);
	do {
		incarnation = get_random_u64();
	} while (!incarnation);
	mx_pdev->lease.device_incarnation = incarnation;
}

bool mx_pdev_get_live(struct mx_pci_dev *mx_pdev)
{
	bool live;

	if (!mx_pdev || !kref_get_unless_zero(&mx_pdev->refcount))
		return false;
	mutex_lock(&mx_pdev->lease.lock);
	live = !mx_pdev->lease.removed;
	mutex_unlock(&mx_pdev->lease.lock);
	if (!live)
		kref_put(&mx_pdev->refcount, mx_pdev_free);
	return live;
}

void mx_pdev_put(struct mx_pci_dev *mx_pdev)
{
	kref_put(&mx_pdev->refcount, mx_pdev_free);
}

void mx_lease_mark_removed(struct mx_pci_dev *mx_pdev)
{
	mutex_lock(&mx_pdev->lease.lock);
	mx_pdev->lease.removed = true;
	mutex_unlock(&mx_pdev->lease.lock);
}

static void mx_file_ctx_free(struct mx_file_ctx *ctx)
{
	struct file *slot_source;
	struct file *slot_proof;
	struct file *lifetime_proof;
	struct mx_pci_dev *mx_pdev;

	if (!ctx || !ctx->mx_pdev)
		return;
	mx_pdev = ctx->mx_pdev;
	mutex_lock(&mx_pdev->lease.lock);
	WARN_ON_ONCE(ctx->transfer_count != 0 || ctx->direct_count != 0);
	mx_lease_sm_release(&mx_pdev->lease.state, &ctx->lease);
	slot_source = ctx->slot_source_file;
	slot_proof = ctx->slot_proof_file;
	lifetime_proof = ctx->lifetime_proof_file;
	ctx->slot_source_file = NULL;
	ctx->slot_proof_file = NULL;
	ctx->lifetime_proof_file = NULL;
	ctx->proof_slot = 0;
	mutex_unlock(&mx_pdev->lease.lock);
	/* Keep the exact inode, identity, generation, and family pinned even when
	 * the last holder exits. Otherwise a crash followed by unlink/recreate can
	 * erase the only durable evidence for an already-dequeued firmware command.
	 * The anchor is released only when this physical-device incarnation dies.
	 */
	if (slot_source)
		fput(slot_source);
	if (slot_proof)
		fput(slot_proof);
	if (lifetime_proof)
		fput(lifetime_proof);
	ctx->magic = 0;
	mx_pdev_put(mx_pdev);
	kfree(ctx);
}

void mx_file_ctx_get(struct mx_file_ctx *ctx)
{
	refcount_inc(&ctx->refs);
}

void mx_file_ctx_put(struct mx_file_ctx *ctx)
{
	if (ctx && refcount_dec_and_test(&ctx->refs))
		mx_file_ctx_free(ctx);
}

int mx_lease_transfer_get(struct mx_file_ctx *ctx, struct mx_transfer *transfer)
{
	struct mx_device_lease *lease;
	int ret = 0;

	if (!ctx || ctx->magic != MAGIC_FILE_CTX || !ctx->mx_pdev || !transfer)
		return -EINVAL;
	lease = &ctx->mx_pdev->lease;
	mutex_lock(&lease->lock);
	if (lease->removed) {
		ret = -ENODEV;
		goto out;
	}
	if (!ctx->direct_count) {
		ret = -EACCES;
		goto out;
	}
	/* Once a workload has published reclamation proofs, detaching them is a
	 * sticky runtime gate.  Rebinding is allowed, but no command may slip into
	 * the normal-close interval without a proof descriptor.
	 */
	if (ctx->proofs_ever_bound &&
	    ctx->lease.profile == MX_LEASE_PROFILE_HUB_WORKLOAD &&
	    !ctx->slot_proof_file) {
		ret = -EACCES;
		goto out;
	}
	if (ctx->proofs_ever_bound &&
	    ctx->lease.profile == MX_LEASE_PROFILE_STANDALONE_WORKLOAD &&
	    !ctx->lifetime_proof_file) {
		ret = -EACCES;
		goto out;
	}
	if (ctx->transfer_count == U32_MAX) {
		ret = -EOVERFLOW;
		goto out;
	}

	mx_file_ctx_get(ctx);
	transfer->owner_ctx = ctx;
	if (ctx->slot_proof_file) {
		get_file(ctx->slot_proof_file);
		transfer->slot_proof_file = ctx->slot_proof_file;
	}
	if (ctx->lifetime_proof_file) {
		get_file(ctx->lifetime_proof_file);
		transfer->lifetime_proof_file = ctx->lifetime_proof_file;
	}
	ctx->transfer_count++;
out:
	mutex_unlock(&lease->lock);
	return ret;
}

void mx_lease_transfer_put(struct mx_transfer *transfer)
{
	struct mx_file_ctx *ctx;
	struct file *slot_proof;
	struct file *lifetime_proof;

	if (!transfer || !transfer->owner_ctx)
		return;
	ctx = transfer->owner_ctx;
	slot_proof = transfer->slot_proof_file;
	lifetime_proof = transfer->lifetime_proof_file;
	transfer->owner_ctx = NULL;
	transfer->slot_proof_file = NULL;
	transfer->lifetime_proof_file = NULL;

	mutex_lock(&ctx->mx_pdev->lease.lock);
	if (WARN_ON_ONCE(ctx->transfer_count == 0))
		ctx->transfer_count = 0;
	else
		ctx->transfer_count--;
	mutex_unlock(&ctx->mx_pdev->lease.lock);
	if (slot_proof)
		fput(slot_proof);
	if (lifetime_proof)
		fput(lifetime_proof);
	mx_file_ctx_put(ctx);
}

bool mx_lease_ioctl_cmd(unsigned int cmd)
{
	return cmd == MX_IOCTL_GET_LEASE_CAPS || cmd == MX_IOCTL_QUERY_LEASE ||
	       cmd == MX_IOCTL_ACQUIRE_LEASE || cmd == MX_IOCTL_TRANSITION_LEASE ||
	       cmd == MX_IOCTL_WORKLOAD_PROOFS ||
	       cmd == MX_IOCTL_ANCHOR_SLOT_DOMAIN;
}

static bool mx_lease_valid_header(u64 magic, u32 version, u32 size,
				  size_t expected_size)
{
	return magic == MX_LEASE_ABI_MAGIC && version == MX_LEASE_ABI_VERSION &&
	       size == expected_size;
}

static bool mx_lease_valid_state_file(struct file *file)
{
	struct inode *inode;

	if (!file || (file->f_mode & (FMODE_READ | FMODE_WRITE)) !=
		    (FMODE_READ | FMODE_WRITE))
		return false;
	inode = file_inode(file);
	return inode && S_ISREG(inode->i_mode) && inode->i_sb &&
	       inode->i_sb->s_magic == TMPFS_MAGIC;
}

static u64 mx_lease_next_generation(struct mx_device_lease *lease)
{
	lease->generation_counter++;
	if (!lease->generation_counter)
		lease->generation_counter++;
	return lease->generation_counter;
}

static u64 mx_lease_holder_grants(const struct mx_file_ctx *ctx, bool anchored)
{
	u64 grants = 0;
	u32 family;

	if (ctx->lease.legacy)
		return MX_LEASE_GRANT_LEGACY;
	if (!ctx->lease.profile)
		return 0;
	if (mx_lease_profile_is_publisher(ctx->lease.profile))
		grants |= MX_LEASE_GRANT_PUBLISHER;
	family = mx_lease_profile_family(ctx->lease.profile);
	if (family == MX_LEASE_FAMILY_HUB)
		grants |= MX_LEASE_GRANT_FAMILY_HUB;
	else if (family == MX_LEASE_FAMILY_STANDALONE)
		grants |= MX_LEASE_GRANT_FAMILY_STANDALONE;
	if (ctx->lease.phase == MX_LEASE_PHASE_QUIESCENT)
		grants |= MX_LEASE_GRANT_QUIESCENT;
	else if (ctx->lease.phase == MX_LEASE_PHASE_WORKLOAD)
		grants |= MX_LEASE_GRANT_WORKLOAD;
	else if (ctx->lease.phase == MX_LEASE_PHASE_REPLACEMENT_CANDIDATE)
		grants |= MX_LEASE_GRANT_REPLACEMENT_CANDIDATE;
	if (anchored)
		grants |= MX_LEASE_GRANT_STATE_ANCHORED;
	if (ctx->slot_proof_file)
		grants |= MX_LEASE_GRANT_SLOT_PROOF_BOUND;
	if (ctx->lifetime_proof_file)
		grants |= MX_LEASE_GRANT_LIFETIME_PROOF_BOUND;
	if (ctx->mx_pdev->lease.slot_domain_path_valid)
		grants |= MX_LEASE_GRANT_SLOT_DOMAIN_ANCHORED;
	return grants;
}

static void mx_lease_fill_acquire_locked(struct mx_file_ctx *ctx,
					 struct mx_lease_acquire *out)
{
	struct mx_device_lease *lease = &ctx->mx_pdev->lease;
	struct mx_lease_sm *sm = &lease->state;

	out->device_incarnation = lease->device_incarnation;
	out->lease_generation = lease->generation;
	out->grants = mx_lease_holder_grants(ctx, lease->state_path_valid);
	out->state_dev = new_encode_dev(lease->state_inode->i_sb->s_dev);
	out->state_ino = lease->state_inode->i_ino;
	out->phase = ctx->lease.phase;
	out->family = sm->family;
	out->publisher_count = sm->publishers;
	out->workload_count = sm->workloads;
	out->family_holder_count = sm->family_holders;
	out->legacy_count = sm->legacy_holders;
}

static long mx_lease_get_caps(struct mx_file_ctx *ctx, unsigned long arg)
{
	struct mx_lease_caps out = {
		.abi_magic = MX_LEASE_ABI_MAGIC,
		.capabilities = MX_LEASE_CAPABILITIES,
		.abi_version = MX_LEASE_ABI_VERSION,
		.struct_size = sizeof(out),
		.max_profile = MX_LEASE_PROFILE_MAX,
		.max_transition = MX_LEASE_TRANSITION_MAX,
		.reserved_hio_qid = ctx->mx_pdev->reserved_hio_qid,
	};

	mutex_lock(&ctx->mx_pdev->lease.lock);
	if (ctx->mx_pdev->lease.removed) {
		mutex_unlock(&ctx->mx_pdev->lease.lock);
		return -ENODEV;
	}
	out.device_incarnation = ctx->mx_pdev->lease.device_incarnation;
	mutex_unlock(&ctx->mx_pdev->lease.lock);
	return copy_to_user((void __user *)arg, &out, sizeof(out)) ? -EFAULT : 0;
}

static long mx_lease_acquire(struct mx_file_ctx *ctx, unsigned long arg)
{
	struct mx_lease_acquire req;
	struct mx_device_lease *lease = &ctx->mx_pdev->lease;
	struct mx_lease_sm old_state;
	struct mx_lease_sm_holder old_holder;
	struct file *state_file;
	bool fresh_anchor = false;
	u32 requested_family;
	long ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (!mx_lease_valid_header(req.abi_magic, req.abi_version,
				   req.struct_size, sizeof(req)) || req.reserved ||
	    req.profile == MX_LEASE_PROFILE_NONE || req.profile > MX_LEASE_PROFILE_MAX ||
	    req.state_fd < 0 || (!req.state_identity[0] && !req.state_identity[1]))
		return -EINVAL;
	if (mx_lease_profile_is_publisher(req.profile) &&
	    ctx->mx_cdev != &ctx->mx_pdev->mx_cdev[MX_CDEV_IOCTL])
		return -EPERM;
	/* The state inode, identity, generation, and device incarnation are
	 * deliberately observable so workloads can verify an attachment.  None of
	 * those values authorize the sole publisher: require host privilege for
	 * fresh publishers, exact-inode restarts, and replacement candidates.
	 */
	if (mx_lease_profile_is_publisher(req.profile) &&
	    !capable(CAP_SYS_RAWIO))
		return -EPERM;
	requested_family = mx_lease_profile_family(req.profile);
	state_file = fget(req.state_fd);
	if (!mx_lease_valid_state_file(state_file)) {
		if (state_file)
			fput(state_file);
		return -EINVAL;
	}
	mutex_lock(&lease->lock);
	if (lease->removed) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!req.expected_device_incarnation ||
	    req.expected_device_incarnation != lease->device_incarnation) {
		ret = -ESTALE;
		goto out_unlock;
	}
	if (!lease->state_path_valid) {
		if (req.expected_lease_generation) {
			ret = -ESTALE;
			goto out_unlock;
		}
		if (!mx_lease_profile_is_publisher(req.profile)) {
			ret = -ENXIO;
			goto out_unlock;
		}
		/* A fresh inode discards any history not represented by an existing
		 * persistent anchor. The publisher privilege check above confines this
		 * operation to explicit host maintenance; container/user-namespace
		 * capabilities are deliberately insufficient.
		 */
		fresh_anchor = true;
	} else if (!req.expected_lease_generation ||
		   req.expected_lease_generation != lease->generation) {
		ret = -ESTALE;
		goto out_unlock;
	} else if (lease->state_inode != file_inode(state_file)) {
		ret = -EXDEV;
		goto out_unlock;
	} else if (lease->state_identity[0] != req.state_identity[0] ||
		   lease->state_identity[1] != req.state_identity[1]) {
		ret = -ESTALE;
		goto out_unlock;
	} else {
		ret = mx_lease_sm_validate_anchored_acquire(&lease->state,
				lease->state_family, req.profile);
		if (ret)
			goto out_unlock;
	}

	old_state = lease->state;
	old_holder = ctx->lease;
	ret = mx_lease_sm_acquire(&lease->state, &ctx->lease, req.profile);
	if (ret)
		goto out_unlock;
	if (fresh_anchor) {
		path_get(&state_file->f_path);
		lease->state_path = state_file->f_path;
		lease->state_path_valid = true;
		lease->state_inode = file_inode(state_file);
		lease->state_identity[0] = req.state_identity[0];
		lease->state_identity[1] = req.state_identity[1];
		lease->state_family = requested_family;
		lease->generation = mx_lease_next_generation(lease);
	}
	mx_lease_fill_acquire_locked(ctx, &req);
	if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
		lease->state = old_state;
		ctx->lease = old_holder;
		if (fresh_anchor) {
			path_put(&lease->state_path);
			lease->state_path_valid = false;
			lease->state_path.mnt = NULL;
			lease->state_path.dentry = NULL;
			lease->state_inode = NULL;
			lease->state_identity[0] = 0;
			lease->state_identity[1] = 0;
			lease->state_family = MX_LEASE_FAMILY_NONE;
			lease->generation = 0;
		}
		ret = -EFAULT;
	}
out_unlock:
	mutex_unlock(&lease->lock);
	if (state_file)
		fput(state_file);
	if (ret)
		return ret;
	return 0;
}

static long mx_lease_transition(struct mx_file_ctx *ctx, unsigned long arg)
{
	struct mx_lease_transition req;
	struct mx_device_lease *lease = &ctx->mx_pdev->lease;
	struct mx_lease_sm *sm = &lease->state;
	struct mx_lease_sm old_state;
	struct mx_lease_sm_holder old_holder;
	long ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (!mx_lease_valid_header(req.abi_magic, req.abi_version,
				   req.struct_size, sizeof(req)) || req.reserved0 ||
	    req.reserved1 || req.operation == MX_LEASE_TRANSITION_NONE ||
	    req.operation > MX_LEASE_TRANSITION_MAX)
		return -EINVAL;
	if (req.operation == MX_LEASE_TRANSITION_ACTIVATE_REPLACEMENT &&
	    !capable(CAP_SYS_RAWIO))
		return -EPERM;

	mutex_lock(&lease->lock);
	if (lease->removed) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!req.expected_device_incarnation ||
	    req.expected_device_incarnation != lease->device_incarnation ||
	    !req.expected_lease_generation ||
	    req.expected_lease_generation != lease->generation) {
		ret = -ESTALE;
		goto out_unlock;
	}
	old_state = *sm;
	old_holder = ctx->lease;
	ret = mx_lease_sm_transition(sm, &ctx->lease, req.operation,
				     ctx->direct_count, ctx->transfer_count,
				     lease->slot_domain_path_valid &&
				     lease->slot_domain_inode);
	if (ret)
		goto out_unlock;
	req.device_incarnation = lease->device_incarnation;
	req.lease_generation = lease->generation;
	req.grants = mx_lease_holder_grants(ctx, lease->state_path_valid);
	req.phase = ctx->lease.phase;
	req.family = sm->family;
	req.publisher_count = sm->publishers;
	req.workload_count = sm->workloads;
	req.family_holder_count = sm->family_holders;
	req.legacy_count = sm->legacy_holders;
	if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
		*sm = old_state;
		ctx->lease = old_holder;
		ret = -EFAULT;
	}
out_unlock:
	mutex_unlock(&lease->lock);
	if (ret)
		return ret;
	return 0;
}

static bool mx_lease_valid_slot_proof(struct file *file, u32 slot)
{
	struct inode *inode;

	if (!file || slot == 0 || slot > MX_LEASE_MAX_SLOT ||
	    (file->f_mode & (FMODE_READ | FMODE_WRITE)) !=
		    (FMODE_READ | FMODE_WRITE) ||
	    READ_ONCE(file->f_pos) !=
		    (loff_t)MX_LEASE_SLOT_MARKER_BASE + slot)
		return false;
	inode = file_inode(file);
	return inode && S_ISREG(inode->i_mode) && inode->i_sb &&
	       inode->i_sb->s_magic == TMPFS_MAGIC;
}

static void mx_lease_init_ofd_lock(struct file_lock *lock, struct file *file,
		unsigned char type, loff_t start, loff_t end)
{
	locks_init_lock(lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	/* 6.9 moved the common fields into struct file_lock_core (c). */
	lock->c.flc_owner = file;
	lock->c.flc_flags = FL_POSIX | FL_OFDLCK;
	lock->c.flc_type = type;
	lock->c.flc_pid = current->tgid;
	lock->c.flc_file = file;
#else
	lock->fl_owner = file;
	lock->fl_flags = FL_POSIX | FL_OFDLCK;
	lock->fl_type = type;
	lock->fl_pid = current->tgid;
	lock->fl_file = file;
#endif
	lock->fl_start = start;
	lock->fl_end = end;
}

static struct file *mx_lease_clone_with_ofd_lock(struct file *source,
		unsigned char type, loff_t start, loff_t end)
{
	struct file *pinned;
	struct file_lock lock;
	int ret;

	pinned = file_clone_open(source);
	if (IS_ERR(pinned))
		return pinned;
	if (file_inode(pinned) != file_inode(source)) {
		fput(pinned);
		return ERR_PTR(-ESTALE);
	}
	mx_lease_init_ofd_lock(&lock, pinned, type, start, end);
	ret = vfs_lock_file(pinned, F_SETLK, &lock, NULL);
	locks_release_private(&lock);
	if (ret) {
		fput(pinned);
		return ERR_PTR(ret == FILE_LOCK_DEFERRED ? -EAGAIN : ret);
	}
	return pinned;
}

/* Retain a separate kernel-only open file description and OFD read lock.
 * The caller only identifies the driver-anchored state inode; it never owns
 * this lock and therefore cannot unlock it. If a fencer already owns the
 * canonical byte, the nonblocking acquisition fails atomically. */
static struct file *mx_lease_pin_slot_ofd_lock(struct file *source, u32 slot)
{
	const loff_t offset = MX_LEASE_SLOT_LIVENESS_OFFSET(slot);

	return mx_lease_clone_with_ofd_lock(source, F_RDLCK, offset, offset);
}

static long mx_lease_anchor_slot_domain(struct mx_file_ctx *ctx,
		unsigned long arg)
{
	struct mx_lease_slot_domain req;
	struct mx_device_lease *lease = &ctx->mx_pdev->lease;
	struct file *domain_file = NULL;
	struct file *exclusive_proof = NULL;
	bool domain_anchored;
	bool same_inode;
	bool fresh_anchor = false;
	long ret = 0;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (!mx_lease_valid_header(req.abi_magic, req.abi_version,
				   req.struct_size, sizeof(req)) ||
	    req.slot_domain_fd < 0 || req.reserved0 || req.slot_domain_dev ||
	    req.slot_domain_ino || req.grants)
		return -EINVAL;
	if (!capable(CAP_SYS_RAWIO) ||
	    ctx->mx_cdev != &ctx->mx_pdev->mx_cdev[MX_CDEV_IOCTL])
		return -EPERM;
	domain_file = fget(req.slot_domain_fd);
	if (!mx_lease_valid_state_file(domain_file)) {
		ret = -EINVAL;
		goto out_files;
	}

	mutex_lock(&lease->lock);
	if (lease->removed) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!req.expected_device_incarnation ||
	    req.expected_device_incarnation != lease->device_incarnation ||
	    !req.expected_lease_generation ||
	    req.expected_lease_generation != lease->generation) {
		ret = -ESTALE;
		goto out_unlock;
	}
	if (lease->slot_domain_path_valid != !!lease->slot_domain_inode) {
		ret = -EUCLEAN;
		goto out_unlock;
	}
	domain_anchored = lease->slot_domain_path_valid;
	same_inode = domain_anchored &&
		lease->slot_domain_inode == file_inode(domain_file);
	ret = mx_lease_sm_validate_slot_domain_anchor(&ctx->lease,
			lease->state_family,
			lease->state_path_valid && lease->state_inode,
			ctx->direct_count, ctx->transfer_count,
			domain_anchored, same_inode);
	if (ret)
		goto out_unlock;
	if (file_inode(domain_file) == lease->state_inode) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (!domain_anchored) {
		/* Hold a whole-file exclusive OFD lock while publishing the first
		 * anchor. Any pre-existing POSIX/OFD record lock makes the fresh
		 * domain ineligible. */
		exclusive_proof = mx_lease_clone_with_ofd_lock(domain_file,
					F_WRLCK, 0, OFFSET_MAX);
		if (IS_ERR(exclusive_proof)) {
			ret = PTR_ERR(exclusive_proof);
			exclusive_proof = NULL;
			goto out_unlock;
		}
		path_get(&domain_file->f_path);
		lease->slot_domain_path = domain_file->f_path;
		lease->slot_domain_path_valid = true;
		lease->slot_domain_inode = file_inode(domain_file);
		fresh_anchor = true;
	}
	req.slot_domain_dev = new_encode_dev(
		lease->slot_domain_inode->i_sb->s_dev);
	req.slot_domain_ino = lease->slot_domain_inode->i_ino;
	req.grants = mx_lease_holder_grants(ctx, lease->state_path_valid);
	if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
		if (fresh_anchor) {
			path_put(&lease->slot_domain_path);
			lease->slot_domain_path_valid = false;
			lease->slot_domain_path.mnt = NULL;
			lease->slot_domain_path.dentry = NULL;
			lease->slot_domain_inode = NULL;
		}
		ret = -EFAULT;
	}
out_unlock:
	mutex_unlock(&lease->lock);
out_files:
	if (exclusive_proof)
		fput(exclusive_proof);
	if (domain_file)
		fput(domain_file);
	return ret;
}

static bool mx_lease_valid_lifetime_proof(struct file *file)
{
	struct inode *inode;

	if (!file || !(file->f_mode & FMODE_WRITE))
		return false;
	inode = file_inode(file);
	return inode && S_ISFIFO(inode->i_mode) && inode->i_sb &&
	       inode->i_sb->s_magic == PIPEFS_MAGIC;
}

static bool mx_lease_proof_request_matches(struct mx_file_ctx *ctx,
					   const struct mx_lease_proofs *req)
{
	struct mx_device_lease *lease = &ctx->mx_pdev->lease;

	return !lease->removed && req->expected_device_incarnation &&
	       req->expected_device_incarnation == lease->device_incarnation &&
	       req->expected_lease_generation &&
	       req->expected_lease_generation == lease->generation &&
	       req->state_identity[0] == lease->state_identity[0] &&
	       req->state_identity[1] == lease->state_identity[1] &&
	       (req->state_identity[0] || req->state_identity[1]) &&
	       lease->state_path_valid;
}

static long mx_lease_bind_proofs(struct mx_file_ctx *ctx,
				 struct mx_lease_proofs *req,
				 unsigned long arg)
{
	struct mx_device_lease *lease = &ctx->mx_pdev->lease;
	struct file *slot_file = NULL;
	struct file *slot_lock_file = NULL;
	struct file *lifetime_file = NULL;
	struct inode *slot_inode = NULL;
	struct inode *lifetime_inode = NULL;
	bool same_binding;
	long ret = 0;

	if (ctx->lease.profile == MX_LEASE_PROFILE_HUB_WORKLOAD) {
		slot_file = fget(req->slot_fd);
		if (!mx_lease_valid_slot_proof(slot_file, req->slot)) {
			ret = -EINVAL;
			goto out_files;
		}
		if (req->lifetime_fd >= 0) {
			lifetime_file = fget(req->lifetime_fd);
			if (!mx_lease_valid_lifetime_proof(lifetime_file)) {
				ret = -EINVAL;
				goto out_files;
			}
		}
	} else if (ctx->lease.profile == MX_LEASE_PROFILE_STANDALONE_WORKLOAD) {
		if (req->slot_fd != -1 || req->slot != 0) {
			ret = -EINVAL;
			goto out_files;
		}
		lifetime_file = fget(req->lifetime_fd);
		if (!mx_lease_valid_lifetime_proof(lifetime_file)) {
			ret = -EINVAL;
			goto out_files;
		}
	} else {
		ret = -EPERM;
		goto out_files;
	}

	mutex_lock(&lease->lock);
	if (!mx_lease_proof_request_matches(ctx, req)) {
		ret = lease->removed ? -ENODEV : -ESTALE;
		goto out_unlock;
	}
	if (ctx->lease.phase != MX_LEASE_PHASE_WORKLOAD) {
		ret = -EACCES;
		goto out_unlock;
	}
	/* Pre-bind bootstrap reads are permitted, but an ambiguous timed-out read
	 * must prevent publication of reclamation proofs and therefore startup.
	 */
	if (ctx->direct_count || ctx->transfer_count) {
		ret = -EBUSY;
		goto out_unlock;
	}

	same_binding = ctx->slot_source_file == slot_file &&
		       ctx->lifetime_proof_file == lifetime_file &&
		       ctx->proof_slot == req->slot;
	if ((ctx->slot_source_file || ctx->slot_proof_file ||
	     ctx->lifetime_proof_file) && !same_binding) {
		ret = -EALREADY;
		goto out_unlock;
	}
	if (!same_binding && slot_file) {
		if (!lease->slot_domain_path_valid || !lease->slot_domain_inode) {
			ret = -EUCLEAN;
			goto out_unlock;
		}
		if (file_inode(slot_file) != lease->slot_domain_inode) {
			ret = -EXDEV;
			goto out_unlock;
		}
	}
	if (!same_binding && slot_file) {
		slot_lock_file = mx_lease_pin_slot_ofd_lock(slot_file, req->slot);
		if (IS_ERR(slot_lock_file)) {
			ret = PTR_ERR(slot_lock_file);
			slot_lock_file = NULL;
			goto out_unlock;
		}
	}

	slot_inode = slot_file ? file_inode(slot_file) : NULL;
	lifetime_inode = lifetime_file ? file_inode(lifetime_file) : NULL;
	req->grants = mx_lease_holder_grants(ctx, lease->state_path_valid) |
		      (slot_file ? MX_LEASE_GRANT_SLOT_PROOF_BOUND : 0) |
		      (lifetime_file ? MX_LEASE_GRANT_LIFETIME_PROOF_BOUND : 0);
	req->slot_dev = slot_inode ? new_encode_dev(slot_inode->i_sb->s_dev) : 0;
	req->slot_ino = slot_inode ? slot_inode->i_ino : 0;
	req->lifetime_dev = lifetime_inode ?
		new_encode_dev(lifetime_inode->i_sb->s_dev) : 0;
	req->lifetime_ino = lifetime_inode ? lifetime_inode->i_ino : 0;
	if (copy_to_user((void __user *)arg, req, sizeof(*req))) {
		ret = -EFAULT;
		goto out_unlock;
	}
	if (!same_binding) {
		ctx->slot_source_file = slot_file;
		ctx->slot_proof_file = slot_lock_file;
		ctx->lifetime_proof_file = lifetime_file;
		ctx->proof_slot = req->slot;
		ctx->proofs_ever_bound = true;
		slot_file = NULL;
		slot_lock_file = NULL;
		lifetime_file = NULL;
	}
out_unlock:
	mutex_unlock(&lease->lock);
out_files:
	if (slot_file)
		fput(slot_file);
	if (slot_lock_file)
		fput(slot_lock_file);
	if (lifetime_file)
		fput(lifetime_file);
	return ret;
}

static long mx_lease_unbind_proofs(struct mx_file_ctx *ctx,
				   struct mx_lease_proofs *req,
				   unsigned long arg)
{
	struct mx_device_lease *lease = &ctx->mx_pdev->lease;
	struct file *slot_source = NULL;
	struct file *slot_proof = NULL;
	struct file *lifetime_file = NULL;
	long ret = 0;

	if (req->slot_fd != -1 || req->lifetime_fd != -1)
		return -EINVAL;
	mutex_lock(&lease->lock);
	if (!mx_lease_proof_request_matches(ctx, req)) {
		ret = lease->removed ? -ENODEV : -ESTALE;
		goto out_unlock;
	}
	if (ctx->lease.profile != MX_LEASE_PROFILE_HUB_WORKLOAD &&
	    ctx->lease.profile != MX_LEASE_PROFILE_STANDALONE_WORKLOAD) {
		ret = -EPERM;
		goto out_unlock;
	}
	if (ctx->slot_proof_file && req->slot != ctx->proof_slot) {
		ret = -ESTALE;
		goto out_unlock;
	}
	if (!ctx->slot_proof_file && req->slot != 0) {
		ret = -EINVAL;
		goto out_unlock;
	}

	req->grants = mx_lease_holder_grants(ctx, lease->state_path_valid) &
		      ~(MX_LEASE_GRANT_SLOT_PROOF_BOUND |
			MX_LEASE_GRANT_LIFETIME_PROOF_BOUND);
	req->slot_dev = 0;
	req->slot_ino = 0;
	req->lifetime_dev = 0;
	req->lifetime_ino = 0;
	if (copy_to_user((void __user *)arg, req, sizeof(*req))) {
		ret = -EFAULT;
		goto out_unlock;
	}
	slot_source = ctx->slot_source_file;
	slot_proof = ctx->slot_proof_file;
	lifetime_file = ctx->lifetime_proof_file;
	ctx->slot_source_file = NULL;
	ctx->slot_proof_file = NULL;
	ctx->lifetime_proof_file = NULL;
	ctx->proof_slot = 0;
out_unlock:
	mutex_unlock(&lease->lock);
	if (slot_source)
		fput(slot_source);
	if (slot_proof)
		fput(slot_proof);
	if (lifetime_file)
		fput(lifetime_file);
	return ret;
}

static long mx_lease_workload_proofs(struct mx_file_ctx *ctx,
				      unsigned long arg)
{
	struct mx_lease_proofs req;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (!mx_lease_valid_header(req.abi_magic, req.abi_version,
				   req.struct_size, sizeof(req)) ||
	    req.operation == MX_LEASE_PROOF_NONE ||
	    req.operation > MX_LEASE_PROOF_MAX || req.grants || req.slot_dev ||
	    req.slot_ino || req.lifetime_dev || req.lifetime_ino ||
	    req.reserved[0] || req.reserved[1] || req.reserved[2])
		return -EINVAL;

	if (req.operation == MX_LEASE_PROOF_BIND)
		return mx_lease_bind_proofs(ctx, &req, arg);
	return mx_lease_unbind_proofs(ctx, &req, arg);
}

static long mx_lease_query(struct mx_file_ctx *ctx, unsigned long arg)
{
	struct mx_lease_query req;
	struct mx_device_lease *lease = &ctx->mx_pdev->lease;
	struct mx_lease_sm *sm = &lease->state;
	long ret = 0;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (!mx_lease_valid_header(req.abi_magic, req.abi_version,
				   req.struct_size, sizeof(req)) ||
	    req.slot_domain_dev || req.slot_domain_ino)
		return -EINVAL;

	mutex_lock(&lease->lock);
	if (lease->removed) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!req.expected_device_incarnation ||
	    req.expected_device_incarnation != lease->device_incarnation) {
		ret = -ESTALE;
		goto out_unlock;
	}
	req.device_incarnation = lease->device_incarnation;
	req.lease_generation = lease->generation;
	req.state_identity[0] = lease->state_identity[0];
	req.state_identity[1] = lease->state_identity[1];
	req.grants = mx_lease_holder_grants(ctx, lease->state_path_valid);
	if (lease->state_inode) {
		req.state_dev = new_encode_dev(lease->state_inode->i_sb->s_dev);
		req.state_ino = lease->state_inode->i_ino;
	} else {
		req.state_dev = 0;
		req.state_ino = 0;
	}
	req.profile = ctx->lease.profile;
	req.phase = ctx->lease.phase;
	req.family = sm->family;
	req.publisher_count = sm->publishers;
	req.workload_count = sm->workloads;
	req.family_holder_count = sm->family_holders;
	req.legacy_count = sm->legacy_holders;
	req.device_state = MX_LEASE_DEVICE_LIVE;
	if (lease->slot_domain_inode) {
		req.slot_domain_dev = new_encode_dev(
			lease->slot_domain_inode->i_sb->s_dev);
		req.slot_domain_ino = lease->slot_domain_inode->i_ino;
	} else {
		req.slot_domain_dev = 0;
		req.slot_domain_ino = 0;
	}
out_unlock:
	mutex_unlock(&lease->lock);
	if (ret)
		return ret;
	return copy_to_user((void __user *)arg, &req, sizeof(req)) ? -EFAULT : 0;
}

long mx_lease_ioctl(struct mx_file_ctx *ctx, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case MX_IOCTL_GET_LEASE_CAPS:
		return mx_lease_get_caps(ctx, arg);
	case MX_IOCTL_QUERY_LEASE:
		return mx_lease_query(ctx, arg);
	case MX_IOCTL_ACQUIRE_LEASE:
		return mx_lease_acquire(ctx, arg);
	case MX_IOCTL_TRANSITION_LEASE:
		return mx_lease_transition(ctx, arg);
	case MX_IOCTL_WORKLOAD_PROOFS:
		return mx_lease_workload_proofs(ctx, arg);
	case MX_IOCTL_ANCHOR_SLOT_DOMAIN:
		return mx_lease_anchor_slot_domain(ctx, arg);
	default:
		return -ENOTTY;
	}
}

int mx_lease_direct_begin(struct mx_file_ctx *ctx)
{
	struct mx_device_lease *lease = &ctx->mx_pdev->lease;
	bool admitted = false;
	int ret;

	mutex_lock(&lease->lock);
	if (lease->removed)
		ret = -ENODEV;
	else {
		ret = mx_lease_sm_admit_direct(&lease->state, &ctx->lease,
				lease->state_path_valid, &ctx->direct_count);
		admitted = !ret;
		if (!ret && ctx->proofs_ever_bound &&
		    ctx->lease.profile == MX_LEASE_PROFILE_HUB_WORKLOAD &&
		    !ctx->slot_proof_file)
			ret = -EACCES;
		if (!ret && ctx->proofs_ever_bound &&
		    ctx->lease.profile == MX_LEASE_PROFILE_STANDALONE_WORKLOAD &&
		    !ctx->lifetime_proof_file)
			ret = -EACCES;
		if (ret && admitted)
			mx_lease_sm_release_direct(&ctx->direct_count);
	}
	mutex_unlock(&lease->lock);
	return ret;
}

void mx_lease_direct_end(struct mx_file_ctx *ctx)
{
	struct mx_device_lease *lease;

	if (!ctx || !ctx->mx_pdev)
		return;
	lease = &ctx->mx_pdev->lease;
	mutex_lock(&lease->lock);
	if (WARN_ON_ONCE(!ctx->direct_count)) {
		mutex_unlock(&lease->lock);
		return;
	}
	mx_lease_sm_release_direct(&ctx->direct_count);
	mutex_unlock(&lease->lock);
}

int mx_lease_authorize_no_completion(struct mx_file_ctx *ctx)
{
	/* There is no completion record or ordered barrier that can prove when this
	 * command stopped touching hardware.  Allowing it would make every later
	 * lease/fence decision a guess, including legacy-to-sandbox handover.
	 */
	return ctx ? -EOPNOTSUPP : -EINVAL;
}

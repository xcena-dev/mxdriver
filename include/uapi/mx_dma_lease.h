/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_MX_DMA_LEASE_H
#define _UAPI_MX_DMA_LEASE_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MX_LEASE_ABI_MAGIC ((__u64)0x4d584c4541534531ULL) /* "MXLEASE1" */
#define MX_LEASE_ABI_VERSION 1U

#define MX_LEASE_CAP_OFD_LIFETIME (1ULL << 0)
#define MX_LEASE_CAP_STATE_INODE_ANCHOR (1ULL << 1)
#define MX_LEASE_CAP_FAMILY_EXCLUSION (1ULL << 2)
#define MX_LEASE_CAP_ATOMIC_TRANSITION (1ULL << 3)
#define MX_LEASE_CAP_HUB_REPLACEMENT (1ULL << 4)
#define MX_LEASE_CAP_LEGACY_EXCLUSION (1ULL << 5)
#define MX_LEASE_CAP_HOT_REMOVE_SAFE (1ULL << 6)
#define MX_LEASE_CAP_PRIVILEGED_REPLACEMENT (1ULL << 7)
#define MX_LEASE_CAP_RESERVED_HIO_QID (1ULL << 8)
/* The exact state inode, identity, generation, and family survive holder=0.
 * They are released only with the physical-device incarnation.
 */
#define MX_LEASE_CAP_PERSISTENT_STATE_ANCHOR (1ULL << 9)
/* Creating the first anchor of a device incarnation requires host
 * CAP_SYS_RAWIO; exact-inode workload attachment does not.
 */
#define MX_LEASE_CAP_PRIVILEGED_FRESH_ANCHOR (1ULL << 10)
/* Every publisher acquisition, including exact-inode restart and replacement
 * candidacy, requires CAP_SYS_RAWIO in the initial user namespace.  A state
 * descriptor and its public identity are not an authority grant.
 */
#define MX_LEASE_CAP_PRIVILEGED_PUBLISHER (1ULL << 11)
/* A workload can bind the exact userspace proof descriptors whose EOF/OFD
 * unlock authorizes resource reclamation.  Hardware transfers retain those
 * open-file descriptions until their terminal completion.
 */
#define MX_LEASE_CAP_WORKLOAD_PROOF_BINDING (1ULL << 12)
/* The driver validates the already-anchored slot-domain inode and retains an
 * independent kernel-owned OFD read lock on its canonical liveness byte. */
#define MX_LEASE_CAP_CANONICAL_SLOT_OFD_PROOF (1ULL << 13)
/* Coordinated controllers share a pre-provisioned local anchor. The anchor
 * is an admission/lifetime identity, not the authoritative device ledger.
 * Initializers remain host-privileged; controllers are NOT sole publishers.
 * Controller-slot proof and application lifetime proof are separate. */
#define MX_LEASE_CAP_COORDINATED_PARTICIPANTS (1ULL << 14)
/* A coordinated workload can pin an independent OFD read lock in its
 * anchored host-local state inode. Recovery can reopen that inode even after
 * every daemon exits; no anonymous-pipe reader/keeper process is required. */
#define MX_LEASE_CAP_WORKLOAD_OFD_LIFETIME (1ULL << 15)
/* The PASSTHRU_CMD ioctl rejects device-wide UnpinAll/UnloadAll for
 * coordinated holders. Ordinary SDK teardown must use owned DPA ranges.
 * This is an SDK-path guard, not isolation from raw HIO/mailbox/BAR access. */
#define MX_LEASE_CAP_SCOPED_MEMORY_COMMANDS (1ULL << 16)

#define MX_LEASE_MAX_WORKLOAD_SLOT 4095U
#define MX_LEASE_WORKLOAD_MARKER_BASE 0x50590000U
#define MX_LEASE_WORKLOAD_LIVENESS_OFFSET(slot) (8192ULL + ((__u64)(slot) * 2ULL) + 1ULL)
#define MX_LEASE_LIFETIME_OFD_FLAG (1ULL << 32)
#define MX_LEASE_LIFETIME_OFD_TAG(slot) (MX_LEASE_LIFETIME_OFD_FLAG | (__u64)(slot))

enum mx_lease_profile
{
    MX_LEASE_PROFILE_NONE = 0,
    MX_LEASE_PROFILE_HUB_PUBLISHER = 1,
    MX_LEASE_PROFILE_HUB_WORKLOAD = 2,
    MX_LEASE_PROFILE_STANDALONE_PUBLISHER = 3,
    MX_LEASE_PROFILE_STANDALONE_WORKLOAD = 4,
    MX_LEASE_PROFILE_COORDINATOR_INITIALIZER = 5,
    MX_LEASE_PROFILE_COORDINATOR_CONTROL = 6,
    MX_LEASE_PROFILE_COORDINATOR_WORKLOAD = 7,
    MX_LEASE_PROFILE_MAX = MX_LEASE_PROFILE_COORDINATOR_WORKLOAD,
};

enum mx_lease_family
{
    MX_LEASE_FAMILY_NONE = 0,
    MX_LEASE_FAMILY_HUB = 1,
    MX_LEASE_FAMILY_STANDALONE = 2,
    MX_LEASE_FAMILY_COORDINATOR = 3,
};

enum mx_lease_phase
{
    MX_LEASE_PHASE_NONE = 0,
    MX_LEASE_PHASE_QUIESCENT = 1,
    MX_LEASE_PHASE_REPLACEMENT_CANDIDATE = 2,
    MX_LEASE_PHASE_WORKLOAD = 3,
    /* Host setup finished. Its descriptor has no direct device access now;
     * independently bound controllers/workloads perform runtime I/O. */
    MX_LEASE_PHASE_COORDINATOR_PREPARED = 4,
};

enum mx_lease_transition_op
{
    MX_LEASE_TRANSITION_NONE = 0,
    MX_LEASE_TRANSITION_TRY_QUIESCENT = 1,
    MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD = 2,
    /* Host recovery only: the kernel additionally requires CAP_SYS_RAWIO. */
    MX_LEASE_TRANSITION_ACTIVATE_REPLACEMENT = 3,
    MX_LEASE_TRANSITION_ACTIVATE_COORDINATOR = 4,
    MX_LEASE_TRANSITION_MAX = MX_LEASE_TRANSITION_ACTIVATE_COORDINATOR,
};

#define MX_LEASE_GRANT_PUBLISHER (1ULL << 0)
#define MX_LEASE_GRANT_FAMILY_HUB (1ULL << 1)
#define MX_LEASE_GRANT_FAMILY_STANDALONE (1ULL << 2)
#define MX_LEASE_GRANT_QUIESCENT (1ULL << 3)
#define MX_LEASE_GRANT_WORKLOAD (1ULL << 4)
#define MX_LEASE_GRANT_REPLACEMENT_CANDIDATE (1ULL << 5)
#define MX_LEASE_GRANT_STATE_ANCHORED (1ULL << 6)
#define MX_LEASE_GRANT_LEGACY (1ULL << 7)
#define MX_LEASE_GRANT_SLOT_PROOF_BOUND (1ULL << 8)
#define MX_LEASE_GRANT_LIFETIME_PROOF_BOUND (1ULL << 9)
#define MX_LEASE_GRANT_SLOT_DOMAIN_ANCHORED (1ULL << 10)
#define MX_LEASE_GRANT_FAMILY_COORDINATOR (1ULL << 11)
#define MX_LEASE_GRANT_COORDINATOR_PREPARED (1ULL << 12)
#define MX_LEASE_GRANT_COORDINATOR_CONTROL (1ULL << 13)

#define MX_LEASE_SLOT_MARKER_BASE 0x50584c00U
#define MX_LEASE_MAX_SLOT 63U
/* Canonical EventHub slot-lock ABI: identity byte = slot * 2 and liveness
 * byte = slot * 2 + 1. The privileged hub publisher anchors this inode in
 * the driver before any workload may bind it. Coordinated controllers use
 * the same local byte layout with slots 0..63; their workloads bind ONLY the
 * application lifetime pipe and must not hold a controller's liveness byte. */
#define MX_LEASE_SLOT_IDENTITY_OFFSET(slot) \
    ((__u64)(slot) * 2ULL)
#define MX_LEASE_SLOT_LIVENESS_OFFSET(slot) \
    (((__u64)(slot) * 2ULL) + 1ULL)

enum mx_lease_proof_op
{
    MX_LEASE_PROOF_NONE = 0,
    MX_LEASE_PROOF_BIND = 1,
    MX_LEASE_PROOF_UNBIND = 2,
    MX_LEASE_PROOF_MAX = MX_LEASE_PROOF_UNBIND,
};

enum mx_lease_device_state
{
    MX_LEASE_DEVICE_LIVE = 1,
    MX_LEASE_DEVICE_REMOVED = 2,
};

struct mx_lease_caps
{
    __u64 abi_magic;
    __u64 capabilities;
    __u64 device_incarnation;
    __u32 abi_version;
    __u32 struct_size;
    __u32 max_profile;
    __u32 max_transition;
    /* -1 means this revision has no driver-owned HIO queue id. */
    __s32 reserved_hio_qid;
    __u32 reserved0;
    __u64 reserved[2];
};

struct mx_lease_acquire
{
    __u64 abi_magic;
    __u64 expected_device_incarnation;
    __u64 expected_lease_generation;
    __u64 state_identity[2];
    __s32 state_fd;
    __u32 profile;
    __u32 abi_version;
    __u32 struct_size;
    __u64 device_incarnation;
    __u64 lease_generation;
    __u64 grants;
    __u64 state_dev;
    __u64 state_ino;
    __u32 phase;
    __u32 family;
    __u32 publisher_count;
    __u32 workload_count;
    __u32 family_holder_count;
    __u32 legacy_count;
    __u64 reserved;
};

struct mx_lease_transition
{
    __u64 abi_magic;
    __u64 expected_device_incarnation;
    __u64 expected_lease_generation;
    __u32 operation;
    __u32 abi_version;
    __u32 struct_size;
    __u32 reserved0;
    __u64 device_incarnation;
    __u64 lease_generation;
    __u64 grants;
    __u32 phase;
    __u32 family;
    __u32 publisher_count;
    __u32 workload_count;
    __u32 family_holder_count;
    __u32 legacy_count;
    __u64 reserved1;
};

struct mx_lease_query
{
    __u64 abi_magic;
    __u64 expected_device_incarnation;
    __u32 abi_version;
    __u32 struct_size;
    __u64 device_incarnation;
    __u64 lease_generation;
    __u64 state_identity[2];
    __u64 grants;
    __u64 state_dev;
    __u64 state_ino;
    __u32 profile;
    __u32 phase;
    __u32 family;
    __u32 publisher_count;
    __u32 workload_count;
    __u32 family_holder_count;
    __u32 legacy_count;
    __u32 device_state;
    __u64 slot_domain_dev;
    __u64 slot_domain_ino;
};

struct mx_lease_slot_domain
{
    __u64 abi_magic;
    __u64 expected_device_incarnation;
    __u64 expected_lease_generation;
    __s32 slot_domain_fd;
    __u32 abi_version;
    __u32 struct_size;
    __u32 reserved0;
    __u64 slot_domain_dev;
    __u64 slot_domain_ino;
    __u64 grants;
};

struct mx_lease_proofs
{
    __u64 abi_magic;
    __u64 expected_device_incarnation;
    __u64 expected_lease_generation;
    __u64 state_identity[2];
    __s32 slot_fd;
    __s32 lifetime_fd;
    __u32 slot;
    __u32 operation;
    __u32 abi_version;
    __u32 struct_size;
    __u64 grants;
    __u64 slot_dev;
    __u64 slot_ino;
    __u64 lifetime_dev;
    __u64 lifetime_ino;
    /* With WORKLOAD_OFD_LIFETIME: reserved[0] may be
     * MX_LEASE_LIFETIME_OFD_TAG(index) for COORDINATOR_WORKLOAD only.
     * Zero preserves the legacy pipe-writer proof. The remaining words stay
     * zero. Bind and unbind must name the same tag. */
    __u64 reserved[3];
};

#define MX_IOCTL_MAGIC 'X'
#define MX_IOCTL_GET_LEASE_CAPS _IOR(MX_IOCTL_MAGIC, 12, struct mx_lease_caps)
#define MX_IOCTL_QUERY_LEASE _IOWR(MX_IOCTL_MAGIC, 13, struct mx_lease_query)
#define MX_IOCTL_ACQUIRE_LEASE _IOWR(MX_IOCTL_MAGIC, 14, struct mx_lease_acquire)
#define MX_IOCTL_TRANSITION_LEASE _IOWR(MX_IOCTL_MAGIC, 15, struct mx_lease_transition)
#define MX_IOCTL_WORKLOAD_PROOFS _IOWR(MX_IOCTL_MAGIC, 16, struct mx_lease_proofs)
#define MX_IOCTL_ANCHOR_SLOT_DOMAIN _IOWR(MX_IOCTL_MAGIC, 17, struct mx_lease_slot_domain)

#endif /* _UAPI_MX_DMA_LEASE_H */

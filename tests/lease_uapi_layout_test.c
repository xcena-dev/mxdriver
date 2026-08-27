// SPDX-License-Identifier: GPL-2.0

#include "../include/uapi/mx_dma_lease.h"

#define OFF(type, member) __builtin_offsetof(type, member)

_Static_assert(sizeof(struct mx_lease_caps) == 64, "caps size");
_Static_assert(OFF(struct mx_lease_caps, reserved_hio_qid) == 40,
	       "caps reserved qid offset");
_Static_assert(OFF(struct mx_lease_caps, reserved) == 48,
	       "caps reserved offset");
_Static_assert(MX_LEASE_CAP_PERSISTENT_STATE_ANCHOR == (1ULL << 9),
	       "persistent anchor capability bit");
_Static_assert(MX_LEASE_CAP_PRIVILEGED_FRESH_ANCHOR == (1ULL << 10),
	       "privileged fresh anchor capability bit");
_Static_assert(MX_LEASE_CAP_PRIVILEGED_PUBLISHER == (1ULL << 11),
	       "privileged publisher capability bit");
_Static_assert(MX_LEASE_CAP_WORKLOAD_PROOF_BINDING == (1ULL << 12),
	       "workload proof capability bit");
_Static_assert(MX_LEASE_CAP_CANONICAL_SLOT_OFD_PROOF == (1ULL << 13),
	       "canonical slot OFD proof capability bit");
_Static_assert(MX_LEASE_GRANT_SLOT_PROOF_BOUND == (1ULL << 8),
	       "slot proof grant bit");
_Static_assert(MX_LEASE_GRANT_LIFETIME_PROOF_BOUND == (1ULL << 9),
	       "lifetime proof grant bit");
_Static_assert(MX_LEASE_GRANT_SLOT_DOMAIN_ANCHORED == (1ULL << 10),
	       "slot domain anchor grant bit");
_Static_assert(MX_LEASE_SLOT_MARKER_BASE == 0x50584c00U,
	       "slot marker ABI");
_Static_assert(MX_LEASE_MAX_SLOT == 63U, "maximum sandbox slot");
_Static_assert(MX_LEASE_SLOT_LIVENESS_OFFSET(1) == 3ULL,
	       "first slot liveness byte");
_Static_assert(MX_LEASE_SLOT_LIVENESS_OFFSET(MX_LEASE_MAX_SLOT) == 127ULL,
	       "last slot liveness byte");

_Static_assert(sizeof(struct mx_lease_acquire) == 128, "acquire size");
_Static_assert(OFF(struct mx_lease_acquire, state_fd) == 40,
	       "acquire state_fd offset");
_Static_assert(OFF(struct mx_lease_acquire, device_incarnation) == 56,
	       "acquire output offset");
_Static_assert(OFF(struct mx_lease_acquire, reserved) == 120,
	       "acquire reserved offset");

_Static_assert(sizeof(struct mx_lease_transition) == 96, "transition size");
_Static_assert(OFF(struct mx_lease_transition, operation) == 24,
	       "transition op offset");
_Static_assert(OFF(struct mx_lease_transition, device_incarnation) == 40,
	       "transition output offset");

_Static_assert(sizeof(struct mx_lease_query) == 128, "query size");
_Static_assert(OFF(struct mx_lease_query, device_incarnation) == 24,
	       "query output offset");
_Static_assert(OFF(struct mx_lease_query, slot_domain_dev) == 112,
	       "query slot-domain device offset");
_Static_assert(OFF(struct mx_lease_query, slot_domain_ino) == 120,
	       "query slot-domain inode offset");

_Static_assert(sizeof(struct mx_lease_slot_domain) == 64,
	       "slot-domain size");
_Static_assert(OFF(struct mx_lease_slot_domain, slot_domain_fd) == 24,
	       "slot-domain fd offset");
_Static_assert(OFF(struct mx_lease_slot_domain, slot_domain_dev) == 40,
	       "slot-domain device offset");
_Static_assert(OFF(struct mx_lease_slot_domain, grants) == 56,
	       "slot-domain grants offset");

_Static_assert(sizeof(struct mx_lease_proofs) == 128, "proofs size");
_Static_assert(OFF(struct mx_lease_proofs, slot_fd) == 40,
	       "proofs slot_fd offset");
_Static_assert(OFF(struct mx_lease_proofs, lifetime_fd) == 44,
	       "proofs lifetime_fd offset");
_Static_assert(OFF(struct mx_lease_proofs, slot) == 48,
	       "proofs slot offset");
_Static_assert(OFF(struct mx_lease_proofs, operation) == 52,
	       "proofs operation offset");
_Static_assert(OFF(struct mx_lease_proofs, grants) == 64,
	       "proofs grants offset");
_Static_assert(OFF(struct mx_lease_proofs, reserved) == 104,
	       "proofs reserved offset");

_Static_assert(_IOC_SIZE(MX_IOCTL_GET_LEASE_CAPS) ==
	       sizeof(struct mx_lease_caps), "caps ioctl size");
_Static_assert(_IOC_SIZE(MX_IOCTL_ACQUIRE_LEASE) ==
	       sizeof(struct mx_lease_acquire), "acquire ioctl size");
_Static_assert(_IOC_SIZE(MX_IOCTL_TRANSITION_LEASE) ==
	       sizeof(struct mx_lease_transition), "transition ioctl size");
_Static_assert(_IOC_SIZE(MX_IOCTL_QUERY_LEASE) ==
	       sizeof(struct mx_lease_query), "query ioctl size");
_Static_assert(_IOC_SIZE(MX_IOCTL_WORKLOAD_PROOFS) ==
	       sizeof(struct mx_lease_proofs), "proofs ioctl size");
_Static_assert(_IOC_NR(MX_IOCTL_WORKLOAD_PROOFS) == 16,
	       "proofs ioctl number");
_Static_assert(_IOC_SIZE(MX_IOCTL_ANCHOR_SLOT_DOMAIN) ==
	       sizeof(struct mx_lease_slot_domain), "slot-domain ioctl size");
_Static_assert(_IOC_NR(MX_IOCTL_ANCHOR_SLOT_DOMAIN) == 17,
	       "slot-domain ioctl number");

int main(void)
{
	return 0;
}

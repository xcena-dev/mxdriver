/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MX_DMA_LEASE_SM_H
#define _MX_DMA_LEASE_SM_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#define MX_SM_U32_MAX U32_MAX
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
#define MX_SM_U32_MAX UINT32_MAX
#endif

#include "include/uapi/mx_dma_lease.h"

struct mx_lease_sm {
	u32 family;
	u32 family_holders;
	u32 publishers;
	u32 workloads;
	u32 legacy_holders;
	bool quiescent;
	/* Persists in the anchored device state when live holders reach zero.
	 * It proves local setup completed, not cross-host/device quiescence. */
	bool coordinator_ready;
};

struct mx_lease_sm_holder {
	u32 profile;
	u32 phase;
	bool legacy;
};

static inline bool mx_lease_sm_valid_lifetime_tag(u32 profile, __u64 tag)
{
	if (!tag)
		return true;
	return profile == MX_LEASE_PROFILE_COORDINATOR_WORKLOAD &&
	       (tag & MX_LEASE_LIFETIME_OFD_FLAG) &&
	       !(tag & ~(MX_LEASE_LIFETIME_OFD_FLAG | MX_LEASE_MAX_WORKLOAD_SLOT));
}

static inline bool mx_lease_profile_is_publisher(u32 profile)
{
	return profile == MX_LEASE_PROFILE_HUB_PUBLISHER ||
	       profile == MX_LEASE_PROFILE_STANDALONE_PUBLISHER ||
	       profile == MX_LEASE_PROFILE_COORDINATOR_INITIALIZER;
}

static inline u32 mx_lease_profile_family(u32 profile)
{
	switch (profile) {
	case MX_LEASE_PROFILE_HUB_PUBLISHER:
	case MX_LEASE_PROFILE_HUB_WORKLOAD:
		return MX_LEASE_FAMILY_HUB;
	case MX_LEASE_PROFILE_STANDALONE_PUBLISHER:
	case MX_LEASE_PROFILE_STANDALONE_WORKLOAD:
		return MX_LEASE_FAMILY_STANDALONE;
	case MX_LEASE_PROFILE_COORDINATOR_INITIALIZER:
	case MX_LEASE_PROFILE_COORDINATOR_CONTROL:
	case MX_LEASE_PROFILE_COORDINATOR_WORKLOAD:
		return MX_LEASE_FAMILY_COORDINATOR;
	default:
		return MX_LEASE_FAMILY_NONE;
	}
}

static inline bool mx_lease_sm_idle(const struct mx_lease_sm *sm)
{
	return sm->family_holders == 0 && sm->publishers == 0 &&
	       sm->workloads == 0 && !sm->quiescent;
}

static inline int mx_lease_sm_authorize_memory_cmd(u32 profile, u32 subopcode)
{
	if (profile > MX_LEASE_PROFILE_MAX)
		return -EINVAL;
	/* Gaia passthrough ABI: 3=UnpinAll, 6=UnloadAll. Range operations,
	 * including Unmap (7), preserve another tenant's memory state. */
	if ((subopcode == 3 || subopcode == 6) &&
	    mx_lease_profile_family(profile) == MX_LEASE_FAMILY_COORDINATOR)
		return -EPERM;
	return 0;
}

/* A persistent state anchor remembers the topology even while its live-holder
 * counters are zero. Only the matching publisher may restart an idle family;
 * workloads attach after that publisher has re-established Quiescent.
 */
static inline int mx_lease_sm_validate_anchored_acquire(
		const struct mx_lease_sm *sm, u32 state_family, u32 profile)
{
	u32 requested_family = mx_lease_profile_family(profile);

	if (!state_family || state_family != requested_family)
		return -EBUSY;
	if (mx_lease_sm_idle(sm) && !mx_lease_profile_is_publisher(profile) &&
	    !(state_family == MX_LEASE_FAMILY_COORDINATOR &&
	      sm->coordinator_ready))
		return -ENXIO;
	return 0;
}

static inline int mx_lease_sm_acquire(struct mx_lease_sm *sm,
				      struct mx_lease_sm_holder *holder,
				      u32 profile)
{
	u32 family = mx_lease_profile_family(profile);
	u32 phase;

	if (!family)
		return -EINVAL;
	if (holder->legacy || sm->legacy_holders)
		return -EBUSY;
	if (holder->profile)
		return holder->profile == profile ? 0 : -EALREADY;
	if (sm->family != MX_LEASE_FAMILY_NONE && sm->family != family)
		return -EBUSY;
	if (sm->family_holders == MX_SM_U32_MAX)
		return -EOVERFLOW;

	switch (profile) {
	case MX_LEASE_PROFILE_COORDINATOR_INITIALIZER:
		if (sm->publishers || sm->quiescent || sm->workloads)
			return -EBUSY;
		phase = MX_LEASE_PHASE_QUIESCENT;
		break;
	case MX_LEASE_PROFILE_COORDINATOR_CONTROL:
	case MX_LEASE_PROFILE_COORDINATOR_WORKLOAD:
		if (!sm->coordinator_ready)
			return -ENXIO;
		if (sm->quiescent || sm->workloads == MX_SM_U32_MAX)
			return sm->quiescent ? -EBUSY : -EOVERFLOW;
		phase = MX_LEASE_PHASE_WORKLOAD;
		break;
	case MX_LEASE_PROFILE_HUB_PUBLISHER:
		if (sm->publishers || sm->quiescent)
			return -EBUSY;
		phase = sm->workloads ? MX_LEASE_PHASE_REPLACEMENT_CANDIDATE :
					MX_LEASE_PHASE_QUIESCENT;
		break;
	case MX_LEASE_PROFILE_STANDALONE_PUBLISHER:
		if (sm->publishers || sm->quiescent || sm->workloads)
			return -EBUSY;
		phase = MX_LEASE_PHASE_QUIESCENT;
		break;
	case MX_LEASE_PROFILE_HUB_WORKLOAD:
	case MX_LEASE_PROFILE_STANDALONE_WORKLOAD:
		if (sm->quiescent || sm->workloads == MX_SM_U32_MAX)
			return sm->quiescent ? -EBUSY : -EOVERFLOW;
		phase = MX_LEASE_PHASE_WORKLOAD;
		break;
	default:
		return -EINVAL;
	}

	if (mx_lease_profile_is_publisher(profile))
		sm->publishers++;
	if (profile == MX_LEASE_PROFILE_COORDINATOR_INITIALIZER)
		sm->coordinator_ready = false;
	if (phase == MX_LEASE_PHASE_QUIESCENT)
		sm->quiescent = true;
	else if (phase == MX_LEASE_PHASE_WORKLOAD)
		sm->workloads++;
	if (!sm->family_holders)
		sm->family = family;
	sm->family_holders++;
	holder->profile = profile;
	holder->phase = phase;
	return 0;
}

/* A live BAR mapping can ring a doorbell without any direct call in flight,
 * so for every quiescence decision it counts like an active direct call. */
static inline int mx_lease_sm_transition(struct mx_lease_sm *sm,
					 struct mx_lease_sm_holder *holder,
					 u32 operation, u32 active_direct,
					 u32 active_transfers, u32 bar_mappings,
					 bool slot_domain_anchored)
{
	if (!mx_lease_profile_is_publisher(holder->profile) || holder->legacy)
		return -EPERM;
	if (holder->profile == MX_LEASE_PROFILE_COORDINATOR_INITIALIZER) {
		if (operation != MX_LEASE_TRANSITION_ACTIVATE_COORDINATOR)
			return -EINVAL;
		if (holder->phase == MX_LEASE_PHASE_COORDINATOR_PREPARED)
			return sm->coordinator_ready ? 0 : -EUCLEAN;
		if (holder->phase != MX_LEASE_PHASE_QUIESCENT)
			return -EINVAL;
		if (!slot_domain_anchored)
			return -ENXIO;
		if (active_direct || active_transfers || bar_mappings)
			return -EBUSY;
		if (!sm->quiescent)
			return -EUCLEAN;
		sm->quiescent = false;
		sm->coordinator_ready = true;
		holder->phase = MX_LEASE_PHASE_COORDINATOR_PREPARED;
		return 0;
	}
	if (holder->profile == MX_LEASE_PROFILE_HUB_PUBLISHER &&
	    (operation == MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD ||
	     operation == MX_LEASE_TRANSITION_ACTIVATE_REPLACEMENT) &&
	    !slot_domain_anchored)
		return -ENXIO;
	if (operation == MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD &&
	    holder->phase == MX_LEASE_PHASE_QUIESCENT &&
	    (active_direct || active_transfers || bar_mappings))
		return -EBUSY;

	switch (operation) {
	case MX_LEASE_TRANSITION_TRY_QUIESCENT:
		if (holder->phase == MX_LEASE_PHASE_QUIESCENT)
			return 0;
		if (holder->phase != MX_LEASE_PHASE_REPLACEMENT_CANDIDATE)
			return -EINVAL;
		if (sm->workloads || sm->quiescent)
			return -EBUSY;
		sm->quiescent = true;
		holder->phase = MX_LEASE_PHASE_QUIESCENT;
		return 0;
	case MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD:
		if (holder->phase == MX_LEASE_PHASE_WORKLOAD)
			return 0;
		if (holder->phase != MX_LEASE_PHASE_QUIESCENT)
			return -EINVAL;
		if (sm->workloads == MX_SM_U32_MAX)
			return -EOVERFLOW;
		if (!sm->quiescent)
			return -EUCLEAN;
		sm->quiescent = false;
		sm->workloads++;
		holder->phase = MX_LEASE_PHASE_WORKLOAD;
		return 0;
	case MX_LEASE_TRANSITION_ACTIVATE_REPLACEMENT:
		if (holder->profile != MX_LEASE_PROFILE_HUB_PUBLISHER ||
		    holder->phase != MX_LEASE_PHASE_REPLACEMENT_CANDIDATE)
			return -EINVAL;
		if (sm->workloads == MX_SM_U32_MAX)
			return -EOVERFLOW;
		sm->workloads++;
		holder->phase = MX_LEASE_PHASE_WORKLOAD;
		return 0;
	default:
		return -EINVAL;
	}
}

static inline void mx_lease_sm_release(struct mx_lease_sm *sm,
				       struct mx_lease_sm_holder *holder)
{
	if (holder->legacy) {
		if (sm->legacy_holders)
			sm->legacy_holders--;
		holder->legacy = false;
		return;
	}
	if (!holder->profile)
		return;
	if (holder->phase == MX_LEASE_PHASE_WORKLOAD && sm->workloads)
		sm->workloads--;
	else if (holder->phase == MX_LEASE_PHASE_QUIESCENT)
		sm->quiescent = false;
	if (mx_lease_profile_is_publisher(holder->profile) && sm->publishers)
		sm->publishers--;
	if (sm->family_holders)
		sm->family_holders--;
	if (!sm->family_holders)
		sm->family = MX_LEASE_FAMILY_NONE;
	holder->profile = MX_LEASE_PROFILE_NONE;
	holder->phase = MX_LEASE_PHASE_NONE;
}

static inline int mx_lease_sm_authorize_direct(struct mx_lease_sm *sm,
					       struct mx_lease_sm_holder *holder)
{
	if (holder->profile) {
		if (holder->phase == MX_LEASE_PHASE_REPLACEMENT_CANDIDATE)
			return -EACCES;
		return holder->phase == MX_LEASE_PHASE_QUIESCENT ||
		       holder->phase == MX_LEASE_PHASE_WORKLOAD ? 0 : -EACCES;
	}
	if (holder->legacy)
		return 0;
	if (!mx_lease_sm_idle(sm))
		return -EBUSY;
	if (sm->legacy_holders == MX_SM_U32_MAX)
		return -EOVERFLOW;
	sm->legacy_holders++;
	holder->legacy = true;
	return 0;
}

static inline int mx_lease_sm_authorize_direct_anchored(
		struct mx_lease_sm *sm, struct mx_lease_sm_holder *holder,
		bool anchored)
{
	if (anchored && !holder->profile && !holder->legacy)
		return -EBUSY;
	return mx_lease_sm_authorize_direct(sm, holder);
}

static inline int mx_lease_sm_admit_direct(struct mx_lease_sm *sm,
		struct mx_lease_sm_holder *holder, bool anchored,
		u32 *active_direct)
{
	int ret;

	if (*active_direct == MX_SM_U32_MAX)
		return -EOVERFLOW;
	ret = mx_lease_sm_authorize_direct_anchored(sm, holder, anchored);
	if (ret)
		return ret;
	(*active_direct)++;
	return 0;
}

static inline void mx_lease_sm_release_direct(u32 *active_direct)
{
	if (*active_direct)
		(*active_direct)--;
}

static inline int mx_lease_sm_validate_slot_domain_anchor(
		const struct mx_lease_sm_holder *holder, u32 state_family,
		bool state_anchored, u32 active_direct, u32 active_transfers,
		u32 bar_mappings, bool domain_anchored, bool same_inode)
{
	if (holder->profile == MX_LEASE_PROFILE_COORDINATOR_INITIALIZER) {
		if (holder->phase != MX_LEASE_PHASE_QUIESCENT ||
		    state_family != MX_LEASE_FAMILY_COORDINATOR || !state_anchored)
			return -EACCES;
		if (active_direct || active_transfers || bar_mappings)
			return -EBUSY;
		return domain_anchored && !same_inode ? -EXDEV : 0;
	}
	if (holder->profile != MX_LEASE_PROFILE_HUB_PUBLISHER ||
	    (holder->phase != MX_LEASE_PHASE_QUIESCENT &&
	     holder->phase != MX_LEASE_PHASE_REPLACEMENT_CANDIDATE) ||
	    state_family != MX_LEASE_FAMILY_HUB || !state_anchored)
		return -EACCES;
	if (active_direct || active_transfers || bar_mappings)
		return -EBUSY;
	if (domain_anchored && !same_inode)
		return -EXDEV;
	return 0;
}

/* Coordinated roles require proof BEFORE their first direct operation.
 * Legacy sandbox profiles retain their pre-bind bootstrap behavior. */
static inline int mx_lease_sm_validate_proofs(u32 profile, bool ever_bound,
		bool slot_bound, bool lifetime_bound)
{
	if ((profile == MX_LEASE_PROFILE_COORDINATOR_CONTROL ||
	     (ever_bound && profile == MX_LEASE_PROFILE_HUB_WORKLOAD)) &&
	    !slot_bound)
		return -EACCES;
	if ((profile == MX_LEASE_PROFILE_COORDINATOR_WORKLOAD ||
	     (ever_bound && profile == MX_LEASE_PROFILE_STANDALONE_WORKLOAD)) &&
	    !lifetime_bound)
		return -EACCES;
	return 0;
}

#endif /* _MX_DMA_LEASE_SM_H */

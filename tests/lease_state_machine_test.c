// SPDX-License-Identifier: GPL-2.0

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../lease_sm.h"

static void assert_invariants(const struct mx_lease_sm *sm)
{
	assert(sm->publishers <= 1);
	assert(sm->workloads <= sm->family_holders);
	assert(sm->publishers <= sm->family_holders);
	assert(!sm->quiescent ||
	       (sm->publishers == 1 && sm->workloads == 0));
	assert(!(sm->legacy_holders && sm->family_holders));
	assert((sm->family == MX_LEASE_FAMILY_NONE) ==
	       (sm->family_holders == 0));
}

static void test_hub_replacement(void)
{
	struct mx_lease_sm sm = {};
	struct mx_lease_sm_holder publisher = {};
	struct mx_lease_sm_holder workload = {};
	struct mx_lease_sm_holder replacement = {};
	struct mx_lease_sm_holder standalone = {};

	assert(mx_lease_sm_acquire(&sm, &publisher,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	assert(publisher.phase == MX_LEASE_PHASE_QUIESCENT);
	assert(sm.quiescent && sm.publishers == 1 && sm.family_holders == 1);
	assert(mx_lease_sm_acquire(&sm, &workload,
				   MX_LEASE_PROFILE_HUB_WORKLOAD) == -EBUSY);
	assert(mx_lease_sm_transition(&sm, &publisher,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == 0);
	assert(!sm.quiescent && sm.workloads == 1);
	assert(mx_lease_sm_acquire(&sm, &workload,
				   MX_LEASE_PROFILE_HUB_WORKLOAD) == 0);
	assert(sm.workloads == 2 && sm.family_holders == 2);
	assert_invariants(&sm);

	mx_lease_sm_release(&sm, &publisher);
	assert(sm.publishers == 0 && sm.workloads == 1 &&
	       sm.family == MX_LEASE_FAMILY_HUB);
	assert(mx_lease_sm_acquire(&sm, &standalone,
				   MX_LEASE_PROFILE_STANDALONE_PUBLISHER) == -EBUSY);
	assert(mx_lease_sm_acquire(&sm, &replacement,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	assert(replacement.phase == MX_LEASE_PHASE_REPLACEMENT_CANDIDATE);
	assert_invariants(&sm);
	assert(mx_lease_sm_authorize_direct(&sm, &replacement) == -EACCES);
	assert(mx_lease_sm_transition(&sm, &replacement,
				      MX_LEASE_TRANSITION_TRY_QUIESCENT, 0, 0, true) == -EBUSY);
	assert(mx_lease_sm_transition(&sm, &replacement,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == -EINVAL);
	assert(mx_lease_sm_authorize_direct(&sm, &replacement) == -EACCES);

	mx_lease_sm_release(&sm, &workload);
	assert(mx_lease_sm_transition(&sm, &replacement,
				      MX_LEASE_TRANSITION_TRY_QUIESCENT, 0, 0, true) == 0);
	assert(replacement.phase == MX_LEASE_PHASE_QUIESCENT && sm.quiescent);
	assert(mx_lease_sm_transition(&sm, &replacement,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == 0);
	assert(mx_lease_sm_transition(&sm, &replacement,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == 0);
	assert(sm.workloads == 1);
	assert_invariants(&sm);
	mx_lease_sm_release(&sm, &replacement);
	assert(mx_lease_sm_idle(&sm) && sm.family == MX_LEASE_FAMILY_NONE);
}

static void test_privileged_replacement_transition(void)
{
	struct mx_lease_sm sm = {};
	struct mx_lease_sm_holder publisher = {};
	struct mx_lease_sm_holder workload = {};
	struct mx_lease_sm_holder replacement = {};

	assert(mx_lease_sm_acquire(&sm, &publisher,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	assert(mx_lease_sm_transition(&sm, &publisher,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == 0);
	assert(mx_lease_sm_acquire(&sm, &workload,
				   MX_LEASE_PROFILE_HUB_WORKLOAD) == 0);
	mx_lease_sm_release(&sm, &publisher);
	assert(mx_lease_sm_acquire(&sm, &replacement,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	assert(replacement.phase == MX_LEASE_PHASE_REPLACEMENT_CANDIDATE);
	assert(mx_lease_sm_transition(&sm, &replacement,
				      MX_LEASE_TRANSITION_ACTIVATE_REPLACEMENT, 0, 0, true) == 0);
	assert(replacement.phase == MX_LEASE_PHASE_WORKLOAD);
	assert(sm.workloads == 2);
	mx_lease_sm_release(&sm, &workload);
	mx_lease_sm_release(&sm, &replacement);
	assert(mx_lease_sm_idle(&sm));
}

static void test_privileged_replacement_after_last_workload(void)
{
	struct mx_lease_sm sm = {};
	struct mx_lease_sm_holder publisher = {};
	struct mx_lease_sm_holder workload = {};
	struct mx_lease_sm_holder replacement = {};

	assert(mx_lease_sm_acquire(&sm, &publisher,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	assert(mx_lease_sm_transition(&sm, &publisher,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == 0);
	assert(mx_lease_sm_acquire(&sm, &workload,
				   MX_LEASE_PROFILE_HUB_WORKLOAD) == 0);
	mx_lease_sm_release(&sm, &publisher);
	assert(mx_lease_sm_acquire(&sm, &replacement,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	mx_lease_sm_release(&sm, &workload);
	assert(sm.workloads == 0);
	assert(mx_lease_sm_transition(&sm, &replacement,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == -EINVAL);
	assert(mx_lease_sm_transition(&sm, &replacement,
				      MX_LEASE_TRANSITION_ACTIVATE_REPLACEMENT, 0, 0, true) == 0);
	assert(sm.workloads == 1);
	assert_invariants(&sm);
	mx_lease_sm_release(&sm, &replacement);
	assert(mx_lease_sm_idle(&sm));
}

static void test_all_cross_family_pairs(void)
{
	static const u32 hub_profiles[] = {
		MX_LEASE_PROFILE_HUB_PUBLISHER,
		MX_LEASE_PROFILE_HUB_WORKLOAD,
	};
	static const u32 standalone_profiles[] = {
		MX_LEASE_PROFILE_STANDALONE_PUBLISHER,
		MX_LEASE_PROFILE_STANDALONE_WORKLOAD,
	};
	size_t i, j;

	for (i = 0; i < sizeof(hub_profiles) / sizeof(hub_profiles[0]); i++) {
		for (j = 0; j < sizeof(standalone_profiles) /
					 sizeof(standalone_profiles[0]); j++) {
			struct mx_lease_sm sm = {};
			struct mx_lease_sm_holder first = {};
			struct mx_lease_sm_holder second = {};

			assert(mx_lease_sm_acquire(&sm, &first,
						   hub_profiles[i]) == 0);
			assert(mx_lease_sm_acquire(&sm, &second,
						   standalone_profiles[j]) == -EBUSY);
			assert_invariants(&sm);
			mx_lease_sm_release(&sm, &first);

			memset(&sm, 0, sizeof(sm));
			memset(&first, 0, sizeof(first));
			memset(&second, 0, sizeof(second));
			assert(mx_lease_sm_acquire(&sm, &first,
						   standalone_profiles[j]) == 0);
			assert(mx_lease_sm_acquire(&sm, &second,
						   hub_profiles[i]) == -EBUSY);
			assert_invariants(&sm);
			mx_lease_sm_release(&sm, &first);
		}
	}
}

static void test_overflow_and_defensive_edges(void)
{
	struct mx_lease_sm_holder holder = {};
	struct mx_lease_sm sm = {
		.family = MX_LEASE_FAMILY_HUB,
		.family_holders = UINT32_MAX,
		.workloads = UINT32_MAX,
	};
	struct mx_lease_sm legacy_sm = { .legacy_holders = UINT32_MAX };
	struct mx_lease_sm_holder legacy = {};
	struct mx_lease_sm publisher_sm = {
		.family = MX_LEASE_FAMILY_HUB,
		.family_holders = 1,
		.publishers = 1,
	};
	struct mx_lease_sm_holder second_publisher = {};
	struct mx_lease_sm invalid_sm = {};
	struct mx_lease_sm_holder invalid = {
		.profile = MX_LEASE_PROFILE_HUB_PUBLISHER,
		.phase = MX_LEASE_PHASE_NONE,
	};

	assert(mx_lease_sm_acquire(&sm, &holder,
				   MX_LEASE_PROFILE_HUB_WORKLOAD) == -EOVERFLOW);
	assert(mx_lease_sm_authorize_direct(&legacy_sm, &legacy) == -EOVERFLOW);
	assert(mx_lease_sm_acquire(&publisher_sm, &second_publisher,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == -EBUSY);
	assert(mx_lease_sm_acquire(&invalid_sm, &holder, 99) == -EINVAL);
	assert(mx_lease_sm_transition(&invalid_sm, &invalid,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == -EINVAL);
}

static void test_release_is_idempotent(void)
{
	struct mx_lease_sm sm = {};
	struct mx_lease_sm_holder holder = {};

	assert(mx_lease_sm_acquire(&sm, &holder,
				   MX_LEASE_PROFILE_HUB_WORKLOAD) == 0);
	mx_lease_sm_release(&sm, &holder);
	assert_invariants(&sm);
	mx_lease_sm_release(&sm, &holder);
	assert_invariants(&sm);
	assert(mx_lease_sm_idle(&sm));
}

static void test_persistent_anchor_restart_contract(void)
{
	struct mx_lease_sm sm = {};
	struct mx_lease_sm_holder publisher = {};
	struct mx_lease_sm_holder unopened = {};

	assert(mx_lease_sm_acquire(&sm, &publisher,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	mx_lease_sm_release(&sm, &publisher);
	assert(mx_lease_sm_idle(&sm));

	assert(mx_lease_sm_validate_anchored_acquire(
		       &sm, MX_LEASE_FAMILY_HUB,
		       MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	assert(mx_lease_sm_validate_anchored_acquire(
		       &sm, MX_LEASE_FAMILY_HUB,
		       MX_LEASE_PROFILE_HUB_WORKLOAD) == -ENXIO);
	assert(mx_lease_sm_validate_anchored_acquire(
		       &sm, MX_LEASE_FAMILY_HUB,
		       MX_LEASE_PROFILE_STANDALONE_PUBLISHER) == -EBUSY);

	assert(mx_lease_sm_authorize_direct_anchored(
		       &sm, &unopened, true) == -EBUSY);
	assert(!unopened.legacy && sm.legacy_holders == 0);
	assert(mx_lease_sm_authorize_direct_anchored(
		       &sm, &unopened, false) == 0);
	assert(unopened.legacy && sm.legacy_holders == 1);
	mx_lease_sm_release(&sm, &unopened);
}

static void test_deterministic_state_walk(void)
{
	struct mx_lease_sm sm = {};
	struct mx_lease_sm_holder holders[4] = {};
	uint32_t seed = 0x501c0deu;
	unsigned int step;

	for (step = 0; step < 200000; step++) {
		struct mx_lease_sm before_sm = sm;
		unsigned int holder_index;
		unsigned int action;
		int ret = 0;

		seed = seed * 1664525u + 1013904223u;
		holder_index = seed % 4;
		action = (seed >> 8) % 13;
		if (action < 5) {
			struct mx_lease_sm_holder before_holder =
				holders[holder_index];

			ret = mx_lease_sm_acquire(&sm, &holders[holder_index],
						  action);
			if (ret) {
				assert(memcmp(&sm, &before_sm, sizeof(sm)) == 0);
				assert(memcmp(&holders[holder_index], &before_holder,
					      sizeof(before_holder)) == 0);
			}
		} else if (action < 9) {
			struct mx_lease_sm_holder before_holder =
				holders[holder_index];

			ret = mx_lease_sm_transition(&sm, &holders[holder_index],
						     action - 5, 0, 0, true);
			if (ret) {
				assert(memcmp(&sm, &before_sm, sizeof(sm)) == 0);
				assert(memcmp(&holders[holder_index], &before_holder,
					      sizeof(before_holder)) == 0);
			}
		} else if (action == 9) {
			struct mx_lease_sm_holder before_holder =
				holders[holder_index];

			ret = mx_lease_sm_authorize_direct(
				&sm, &holders[holder_index]);
			if (ret) {
				assert(memcmp(&sm, &before_sm, sizeof(sm)) == 0);
				assert(memcmp(&holders[holder_index], &before_holder,
					      sizeof(before_holder)) == 0);
			}
		} else {
			mx_lease_sm_release(&sm, &holders[holder_index]);
		}
		assert_invariants(&sm);
	}
	for (step = 0; step < 4; step++)
		mx_lease_sm_release(&sm, &holders[step]);
	assert_invariants(&sm);
	assert(mx_lease_sm_idle(&sm));
}

static void test_standalone_strict(void)
{
	struct mx_lease_sm sm = {};
	struct mx_lease_sm_holder publisher = {};
	struct mx_lease_sm_holder workload = {};
	struct mx_lease_sm_holder replacement = {};

	assert(mx_lease_sm_acquire(&sm, &publisher,
				   MX_LEASE_PROFILE_STANDALONE_PUBLISHER) == 0);
	assert(publisher.phase == MX_LEASE_PHASE_QUIESCENT);
	assert(mx_lease_sm_transition(&sm, &publisher,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == 0);
	assert(mx_lease_sm_acquire(&sm, &workload,
				   MX_LEASE_PROFILE_STANDALONE_WORKLOAD) == 0);
	mx_lease_sm_release(&sm, &publisher);
	assert(mx_lease_sm_acquire(&sm, &replacement,
				   MX_LEASE_PROFILE_STANDALONE_PUBLISHER) == -EBUSY);
	mx_lease_sm_release(&sm, &workload);
	assert(mx_lease_sm_idle(&sm));
}

static void test_legacy_exclusion(void)
{
	struct mx_lease_sm sm = {};
	struct mx_lease_sm_holder unopened = {};
	struct mx_lease_sm_holder legacy = {};
	struct mx_lease_sm_holder hub = {};

	/* Mere open is deliberately inert. The first direct operation registers legacy. */
	assert(!unopened.legacy && mx_lease_sm_idle(&sm));
	assert(mx_lease_sm_authorize_direct(&sm, &legacy) == 0);
	assert(legacy.legacy && sm.legacy_holders == 1);
	assert(mx_lease_sm_authorize_direct(&sm, &legacy) == 0);
	assert(sm.legacy_holders == 1);
	assert(mx_lease_sm_acquire(&sm, &hub,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == -EBUSY);
	mx_lease_sm_release(&sm, &legacy);
	assert(sm.legacy_holders == 0);
	assert(mx_lease_sm_acquire(&sm, &hub,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	assert(mx_lease_sm_authorize_direct(&sm, &unopened) == -EBUSY);
	mx_lease_sm_release(&sm, &hub);
}

static void test_idempotence_and_invalid_transitions(void)
{
	struct mx_lease_sm sm = {};
	struct mx_lease_sm_holder hub = {};
	struct mx_lease_sm_holder app = {};

	assert(mx_lease_sm_acquire(&sm, &hub,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	assert(mx_lease_sm_acquire(&sm, &hub,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	assert(sm.publishers == 1 && sm.family_holders == 1);
	assert(mx_lease_sm_acquire(&sm, &hub,
				   MX_LEASE_PROFILE_HUB_WORKLOAD) == -EALREADY);
	assert(mx_lease_sm_transition(&sm, &hub, 99, 0, 0, true) == -EINVAL);
	assert(mx_lease_sm_transition(&sm, &hub,
				      MX_LEASE_TRANSITION_TRY_QUIESCENT, 0, 0, true) == 0);
	assert(mx_lease_sm_transition(&sm, &hub,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == 0);
	assert(mx_lease_sm_acquire(&sm, &app,
				   MX_LEASE_PROFILE_HUB_WORKLOAD) == 0);
	assert(mx_lease_sm_transition(&sm, &app,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == -EPERM);
	mx_lease_sm_release(&sm, &app);
	mx_lease_sm_release(&sm, &hub);
	assert(mx_lease_sm_idle(&sm));
}

static void test_direct_admission_blocks_activation(void)
{
	struct mx_lease_sm sm = {};
	struct mx_lease_sm_holder publisher = {};
	u32 active_direct = 0;

	assert(mx_lease_sm_acquire(&sm, &publisher,
				   MX_LEASE_PROFILE_HUB_PUBLISHER) == 0);
	assert(mx_lease_sm_transition(&sm, &publisher,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD,
				      0, 0, false) == -ENXIO);
	assert(publisher.phase == MX_LEASE_PHASE_QUIESCENT && sm.quiescent);
	assert(mx_lease_sm_admit_direct(&sm, &publisher, true,
					&active_direct) == 0);
	assert(active_direct == 1);
	assert(mx_lease_sm_transition(&sm, &publisher,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD,
				      active_direct, 0, true) == -EBUSY);
	assert(publisher.phase == MX_LEASE_PHASE_QUIESCENT && sm.quiescent);

	mx_lease_sm_release_direct(&active_direct);
	assert(active_direct == 0);
	/* Models a nowait/zombie command after its syscall admission ended. */
	assert(mx_lease_sm_transition(&sm, &publisher,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD,
				      active_direct, 1, true) == -EBUSY);
	assert(publisher.phase == MX_LEASE_PHASE_QUIESCENT && sm.quiescent);
	assert(mx_lease_sm_transition(&sm, &publisher,
				      MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD,
				      0, 0, true) == 0);
	mx_lease_sm_release(&sm, &publisher);
	assert(mx_lease_sm_idle(&sm));
}

static void test_slot_domain_anchor_gate(void)
{
	struct mx_lease_sm_holder publisher = {
		.profile = MX_LEASE_PROFILE_HUB_PUBLISHER,
		.phase = MX_LEASE_PHASE_QUIESCENT,
	};

	assert(mx_lease_sm_validate_slot_domain_anchor(&publisher,
			MX_LEASE_FAMILY_HUB, true, 0, 0, false, false) == 0);
	assert(mx_lease_sm_validate_slot_domain_anchor(&publisher,
			MX_LEASE_FAMILY_HUB, true, 0, 0, true, true) == 0);
	assert(mx_lease_sm_validate_slot_domain_anchor(&publisher,
			MX_LEASE_FAMILY_HUB, true, 0, 0, true, false) == -EXDEV);
	assert(mx_lease_sm_validate_slot_domain_anchor(&publisher,
			MX_LEASE_FAMILY_HUB, true, 1, 0, false, false) == -EBUSY);
	assert(mx_lease_sm_validate_slot_domain_anchor(&publisher,
			MX_LEASE_FAMILY_HUB, true, 0, 1, false, false) == -EBUSY);
	publisher.phase = MX_LEASE_PHASE_WORKLOAD;
	assert(mx_lease_sm_validate_slot_domain_anchor(&publisher,
			MX_LEASE_FAMILY_HUB, true, 0, 0, false, false) == -EACCES);
	publisher.phase = MX_LEASE_PHASE_REPLACEMENT_CANDIDATE;
	assert(mx_lease_sm_validate_slot_domain_anchor(&publisher,
			MX_LEASE_FAMILY_HUB, true, 0, 0, false, false) == 0);
}

static void test_canonical_slot_lock_offsets(void)
{
	assert(MX_LEASE_SLOT_LIVENESS_OFFSET(1) == 3ULL);
	assert(MX_LEASE_SLOT_LIVENESS_OFFSET(MX_LEASE_MAX_SLOT) == 127ULL);
}

int main(void)
{
	test_hub_replacement();
	test_privileged_replacement_transition();
	test_privileged_replacement_after_last_workload();
	test_all_cross_family_pairs();
	test_overflow_and_defensive_edges();
	test_release_is_idempotent();
	test_persistent_anchor_restart_contract();
	test_deterministic_state_walk();
	test_standalone_strict();
	test_legacy_exclusion();
	test_idempotence_and_invalid_transitions();
	test_direct_admission_blocks_activation();
	test_slot_domain_anchor_gate();
	test_canonical_slot_lock_offsets();
	puts("lease state-machine tests: PASS");
	return 0;
}

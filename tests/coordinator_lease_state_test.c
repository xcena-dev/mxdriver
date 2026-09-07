/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdio.h>

#include "../lease_sm.h"

static void prepare(struct mx_lease_sm *state)
{
	struct mx_lease_sm_holder initializer = {0};

	assert(mx_lease_sm_valid_lifetime_tag(MX_LEASE_PROFILE_COORDINATOR_WORKLOAD, 0));
	assert(mx_lease_sm_valid_lifetime_tag(MX_LEASE_PROFILE_COORDINATOR_WORKLOAD,
		MX_LEASE_LIFETIME_OFD_TAG(0)));
	assert(mx_lease_sm_valid_lifetime_tag(MX_LEASE_PROFILE_COORDINATOR_WORKLOAD,
		MX_LEASE_LIFETIME_OFD_TAG(MX_LEASE_MAX_WORKLOAD_SLOT)));
	assert(!mx_lease_sm_valid_lifetime_tag(MX_LEASE_PROFILE_COORDINATOR_WORKLOAD,
		MX_LEASE_LIFETIME_OFD_TAG(MX_LEASE_MAX_WORKLOAD_SLOT + 1)));
	assert(!mx_lease_sm_valid_lifetime_tag(MX_LEASE_PROFILE_COORDINATOR_WORKLOAD, 1));
	assert(!mx_lease_sm_valid_lifetime_tag(MX_LEASE_PROFILE_COORDINATOR_CONTROL,
		MX_LEASE_LIFETIME_OFD_TAG(0)));
	assert(!mx_lease_sm_valid_lifetime_tag(MX_LEASE_PROFILE_HUB_WORKLOAD,
		MX_LEASE_LIFETIME_OFD_TAG(0)));

	assert(mx_lease_sm_acquire(state, &initializer,
		MX_LEASE_PROFILE_COORDINATOR_INITIALIZER) == 0);
	assert(initializer.phase == MX_LEASE_PHASE_QUIESCENT);
	assert(!state->coordinator_ready);
	assert(mx_lease_sm_transition(state, &initializer,
		MX_LEASE_TRANSITION_ACTIVATE_COORDINATOR, 0, 0, false) == -ENXIO);
	assert(mx_lease_sm_transition(state, &initializer,
		MX_LEASE_TRANSITION_ACTIVATE_COORDINATOR, 1, 0, true) == -EBUSY);
	assert(mx_lease_sm_transition(state, &initializer,
		MX_LEASE_TRANSITION_ACTIVATE_COORDINATOR, 0, 1, true) == -EBUSY);
	assert(mx_lease_sm_transition(state, &initializer,
		MX_LEASE_TRANSITION_ACTIVATE_WORKLOAD, 0, 0, true) == -EINVAL);
	assert(mx_lease_sm_transition(state, &initializer,
		MX_LEASE_TRANSITION_ACTIVATE_COORDINATOR, 0, 0, true) == 0);
	assert(state->coordinator_ready && !state->quiescent);
	assert(initializer.phase == MX_LEASE_PHASE_COORDINATOR_PREPARED);
	assert(mx_lease_sm_authorize_direct(state, &initializer) == -EACCES);
	assert(mx_lease_sm_transition(state, &initializer,
		MX_LEASE_TRANSITION_ACTIVATE_COORDINATOR, 0, 0, true) == 0);
	mx_lease_sm_release(state, &initializer);
	assert(mx_lease_sm_idle(state));
	assert(state->coordinator_ready);
}

static void concurrent_controllers_and_workload(void)
{
	struct mx_lease_sm state = {0};
	struct mx_lease_sm_holder a = {0}, b = {0}, workload = {0}, other = {0};

	assert(mx_lease_sm_acquire(&state, &a,
		MX_LEASE_PROFILE_COORDINATOR_CONTROL) == -ENXIO);
	assert(mx_lease_sm_acquire(&state, &workload,
		MX_LEASE_PROFILE_COORDINATOR_WORKLOAD) == -ENXIO);
	prepare(&state);
	assert(mx_lease_sm_validate_anchored_acquire(&state,
		MX_LEASE_FAMILY_COORDINATOR, MX_LEASE_PROFILE_HUB_PUBLISHER) == -EBUSY);
	assert(mx_lease_sm_validate_anchored_acquire(&state,
		MX_LEASE_FAMILY_COORDINATOR, MX_LEASE_PROFILE_COORDINATOR_CONTROL) == 0);
	assert(mx_lease_sm_acquire(&state, &a,
		MX_LEASE_PROFILE_COORDINATOR_CONTROL) == 0);
	assert(mx_lease_sm_acquire(&state, &b,
		MX_LEASE_PROFILE_COORDINATOR_CONTROL) == 0);
	assert(state.publishers == 0); /* no fixed/sole publisher */
	assert(state.workloads == 2 && state.family_holders == 2);
	assert(mx_lease_sm_acquire(&state, &workload,
		MX_LEASE_PROFILE_COORDINATOR_WORKLOAD) == 0);
	assert(mx_lease_sm_acquire(&state, &other,
		MX_LEASE_PROFILE_COORDINATOR_INITIALIZER) == -EBUSY);
	assert(mx_lease_sm_authorize_direct_anchored(&state, &other, true) == -EBUSY);
	mx_lease_sm_release(&state, &a);
	assert(mx_lease_sm_authorize_direct(&state, &b) == 0);
	assert(mx_lease_sm_authorize_direct(&state, &workload) == 0);
	assert(mx_lease_sm_transition(&state, &b,
		MX_LEASE_TRANSITION_TRY_QUIESCENT, 0, 0, true) == -EPERM);
	mx_lease_sm_release(&state, &b);
	assert(mx_lease_sm_acquire(&state, &other,
		MX_LEASE_PROFILE_COORDINATOR_CONTROL) == 0);
	mx_lease_sm_release(&state, &other);
	mx_lease_sm_release(&state, &workload);
	assert(mx_lease_sm_idle(&state));
	assert(state.coordinator_ready);
}

static void incomplete_setup_and_proof_separation(void)
{
	struct mx_lease_sm state = {0};
	struct mx_lease_sm_holder initializer = {0}, control = {0};

	assert(mx_lease_sm_acquire(&state, &initializer,
		MX_LEASE_PROFILE_COORDINATOR_INITIALIZER) == 0);
	assert(mx_lease_sm_validate_slot_domain_anchor(&initializer,
		MX_LEASE_FAMILY_COORDINATOR, true, 0, 0, false, false) == 0);
	mx_lease_sm_release(&state, &initializer); /* crash before setup completion */
	assert(!state.coordinator_ready);
	assert(mx_lease_sm_acquire(&state, &control,
		MX_LEASE_PROFILE_COORDINATOR_CONTROL) == -ENXIO);

	assert(mx_lease_sm_validate_proofs(MX_LEASE_PROFILE_COORDINATOR_CONTROL,
		false, false, true) == -EACCES);
	assert(mx_lease_sm_validate_proofs(MX_LEASE_PROFILE_COORDINATOR_CONTROL,
		false, true, false) == 0);
	assert(mx_lease_sm_validate_proofs(MX_LEASE_PROFILE_COORDINATOR_WORKLOAD,
		false, true, false) == -EACCES);
	assert(mx_lease_sm_validate_proofs(MX_LEASE_PROFILE_COORDINATOR_WORKLOAD,
		false, false, true) == 0);
	assert(mx_lease_sm_validate_proofs(MX_LEASE_PROFILE_COORDINATOR_CONTROL,
		true, false, false) == -EACCES);
	assert(mx_lease_sm_validate_proofs(MX_LEASE_PROFILE_COORDINATOR_WORKLOAD,
		true, false, false) == -EACCES);
	/* Existing bootstrap behavior is unchanged outside the new profiles. */
	assert(mx_lease_sm_validate_proofs(MX_LEASE_PROFILE_HUB_WORKLOAD,
		false, false, false) == 0);
}

int main(void)
{
	concurrent_controllers_and_workload();
	incomplete_setup_and_proof_separation();
	puts("coordinator lease state tests: PASS");
	return 0;
}

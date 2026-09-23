// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../include/uapi/mx_dma_lease.h"

/* F_OFD_GETLK reports the type of a CONFLICTING lock held by another open
 * file description, or F_UNLCK when a lock of `type` could be taken. */
static short probe_ofd_lock(int fd, short type, off_t offset)
{
	struct flock lock = {
		.l_type = type,
		.l_whence = SEEK_SET,
		.l_start = offset,
		.l_len = 1,
		.l_pid = 0,
	};

	assert(fcntl(fd, F_OFD_GETLK, &lock) == 0);
	return lock.l_type;
}

static int set_ofd_lock(int fd, short type, off_t offset)
{
	struct flock lock = {
		.l_type = type,
		.l_whence = SEEK_SET,
		.l_start = offset,
		.l_len = 1,
		.l_pid = 0,
	};

	return fcntl(fd, F_OFD_SETLK, &lock);
}

int main(void)
{
	const off_t offset = (off_t)MX_LEASE_SLOT_LIVENESS_OFFSET(7);
	char path[] = "/tmp/mx-lease-ofd.XXXXXX";
	int publisher;
	int workload_source;
	int driver_clone;
	int race_clone;
	int contender;

	publisher = mkstemp(path);
	assert(publisher >= 0);
	workload_source = open(path, O_RDWR | O_CLOEXEC);
	driver_clone = open(path, O_RDWR | O_CLOEXEC);
	race_clone = open(path, O_RDWR | O_CLOEXEC);
	contender = open(path, O_RDWR | O_CLOEXEC);
	assert(workload_source >= 0 && driver_clone >= 0 && race_clone >= 0 &&
	       contender >= 0);
	assert(unlink(path) == 0);

	/* A fresh slot domain accepts an exclusive probe before anchoring. */
	assert(set_ofd_lock(contender, F_WRLCK, 0) == 0);
	assert(set_ofd_lock(contender, F_UNLCK, 0) == 0);

	/* Models the kernel-owned clone installed by proof binding. */
	assert(set_ofd_lock(driver_clone, F_RDLCK, offset) == 0);
	errno = 0;
	assert(set_ofd_lock(contender, F_WRLCK, offset) == -1);
	assert(errno == EACCES || errno == EAGAIN);

	/* Closing the delegated source does not release the driver's proof. */
	assert(close(workload_source) == 0);
	errno = 0;
	assert(set_ofd_lock(contender, F_WRLCK, offset) == -1);
	assert(errno == EACCES || errno == EAGAIN);

	/* Workload teardown drops the clone; publisher's unlocked fd stays open. */
	assert(close(driver_clone) == 0);
	assert(set_ofd_lock(contender, F_WRLCK, offset) == 0);
	errno = 0;
	assert(set_ofd_lock(race_clone, F_RDLCK, offset) == -1);
	assert(errno == EACCES || errno == EAGAIN);
	assert(set_ofd_lock(contender, F_UNLCK, offset) == 0);
	assert(fcntl(publisher, F_GETFD) >= 0);

	assert(close(contender) == 0);
	assert(close(race_clone) == 0);
	assert(close(publisher) == 0);

	/* The slot identity proof the driver now enforces at bind time. The
	 * claimant takes identity+liveness exclusively and downgrades only the
	 * liveness byte (makeLive); the identity byte stays exclusive for the
	 * claim's lifetime. Probing from the claimant's own description finds no
	 * conflict, probing from a fresh description (the kernel's clone) does:
	 * both together prove the exclusive lock is the caller's own. */
	{
		const off_t identity = (off_t)MX_LEASE_SLOT_IDENTITY_OFFSET(7);
		char again[] = "/tmp/mx-lease-ofd.XXXXXX";
		int claimant, kernel_clone, impostor;
		struct flock both = {
			.l_type = F_WRLCK, .l_whence = SEEK_SET,
			.l_start = identity, .l_len = 2, .l_pid = 0,
		};

		claimant = mkstemp(again);
		assert(claimant >= 0);
		kernel_clone = open(again, O_RDWR | O_CLOEXEC);
		impostor = open(again, O_RDWR | O_CLOEXEC);
		assert(kernel_clone >= 0 && impostor >= 0);
		assert(unlink(again) == 0);

		/* Nobody has claimed: the clone probe sees no conflict, so the
		 * driver must refuse -- a marker in f_pos alone is not a claim. */
		assert(probe_ofd_lock(kernel_clone, F_WRLCK, identity) == F_UNLCK);

		assert(fcntl(claimant, F_OFD_SETLK, &both) == 0);
		assert(set_ofd_lock(claimant, F_RDLCK, offset) == 0);	/* makeLive */
		/* Own description: no conflict. Clone: conflict. Proof holds. */
		assert(probe_ofd_lock(claimant, F_WRLCK, identity) == F_UNLCK);
		assert(probe_ofd_lock(kernel_clone, F_WRLCK, identity) == F_WRLCK);
		/* The driver's liveness read pin still coexists with the claim. */
		assert(set_ofd_lock(kernel_clone, F_RDLCK, offset) == 0);

		/* A second claimant of the same slot fails the proof: from ITS own
		 * description the identity byte conflicts, so it is a stranger's. */
		assert(probe_ofd_lock(impostor, F_WRLCK, identity) == F_WRLCK);
		errno = 0;
		assert(fcntl(impostor, F_OFD_SETLK, &both) == -1);
		assert(errno == EACCES || errno == EAGAIN);

		assert(close(impostor) == 0);
		assert(close(kernel_clone) == 0);
		assert(close(claimant) == 0);
	}
	puts("lease OFD lifetime tests: PASS");
	return 0;
}

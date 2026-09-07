// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../include/uapi/mx_dma_lease.h"

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
	puts("lease OFD lifetime tests: PASS");
	return 0;
}

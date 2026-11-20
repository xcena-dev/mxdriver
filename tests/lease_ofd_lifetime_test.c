// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../include/uapi/mx_dma_lease.h"

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

	assert(set_ofd_lock(contender, F_WRLCK, 0) == 0);
	assert(set_ofd_lock(contender, F_UNLCK, 0) == 0);

	assert(set_ofd_lock(driver_clone, F_RDLCK, offset) == 0);
	errno = 0;
	assert(set_ofd_lock(contender, F_WRLCK, offset) == -1);
	assert(errno == EACCES || errno == EAGAIN);

	assert(close(workload_source) == 0);
	errno = 0;
	assert(set_ofd_lock(contender, F_WRLCK, offset) == -1);
	assert(errno == EACCES || errno == EAGAIN);

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

		assert(probe_ofd_lock(kernel_clone, F_WRLCK, identity) == F_UNLCK);

		assert(fcntl(claimant, F_OFD_SETLK, &both) == 0);
		assert(set_ofd_lock(claimant, F_RDLCK, offset) == 0);
		assert(probe_ofd_lock(claimant, F_WRLCK, identity) == F_UNLCK);
		assert(probe_ofd_lock(kernel_clone, F_WRLCK, identity) == F_WRLCK);
		assert(set_ofd_lock(kernel_clone, F_RDLCK, offset) == 0);

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

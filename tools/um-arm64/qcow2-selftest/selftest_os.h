/* SPDX-License-Identifier: GPL-2.0 */
/*
 * os_* wrappers for the host-side qcow2 selftest.  The engine calls only
 * these two; the semantics mirror arch/um/os-Linux/file.c: bytes moved on
 * success, -errno on failure.
 */
#ifndef __SELFTEST_OS_H__
#define __SELFTEST_OS_H__

#include <unistd.h>
#include <errno.h>

static inline int os_pread_file(int fd, void *buf, int len,
				unsigned long long offset)
{
	ssize_t n = pread(fd, buf, len, offset);

	return n < 0 ? -errno : (int)n;
}

static inline int os_pwrite_file(int fd, const void *buf, int count,
				 unsigned long long offset)
{
	ssize_t n = pwrite(fd, buf, count, offset);

	return n < 0 ? -errno : (int)n;
}

#endif /* __SELFTEST_OS_H__ */
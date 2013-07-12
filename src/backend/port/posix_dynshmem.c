/*-------------------------------------------------------------------------
 *
 * posix_shmem.c
 *	  Implement dynamic shared memory using shm_open()
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/port/posix_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "storage/pg_shmem.h"

/*
 * Allocate a new shared memory segment of the specified length and attach it
 * to the current process at a system-chosen address.
 */
void
dsm_create(DynElephant *x, Size len)
{
	int fd;

	/*
	 * We presume startup destroyed any lingering memory objects, so we use
	 * O_EXCL to detect unexpected reuse.
	 */
	fd = shm_open("/PG", O_RDWR | O_CREAT | O_EXCL, IPCProtection);
	if (fd == -1)
		elog(ERROR, "shm_open failed: %m");

	if (ftruncate(fd, len) != 0)
		elog(ERROR, "ftruncate failed: %m");

	x->addr = mmap(NULL, len,
				   PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_HASSEMAPHORE,
				   fd, 0);
	if (x->addr == MAP_FAILED)
		elog(ERROR, "mmap failed: %m");

	/*
	 * XXX if we fail here, we need to reclaim the segment.  Probably need to
	 * associate segments with resource owners.
	 */
	if (close(fd) != 0)
		elog(ERROR, "close failed: %m");

	x->len = len;
}

/*
 * Only the owning process should call after detaching itself.  After this is
 * done, later attempts to attach to the same segment will fail.  It's
 * unspecified
 */
void
dsm_destroy(DynElephant *x)
{
	if (munmap(x->addr, x->len) != 0)
		elog(ERROR, "munmap failed: %m");

	if (shm_unlink("/PG") == -1)
		elog(ERROR, "shm_unlink failed: %m");
}

/*
 * Attempt to attach a dynamic shared memory area to the current process.
 * This should not normally be called in the owning process, but one can do so
 */
void
dsm_attach(DynElephant *x)
{
	int fd;
	void *addr;

	fd = shm_open("/PG", O_RDWR, 0);
	if (fd == -1)
		elog(ERROR, "shm_open failed: %m");

	addr = mmap(x->addr, x->len,
				PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_HASSEMAPHORE,
				fd, 0);
	if (addr == MAP_FAILED)
		elog(ERROR, "mmap failed: %m");
	if (addr != x->addr)
		elog(ERROR, "dsm_attach: address mismatch");

	if (close(fd) != 0)
		elog(ERROR, "close failed: %m");
}

/*
 * Undo a dsm_attach().  This frees the address space and allows the
 * allocation to be freed when all users have done so.
 */
void
dsm_detach(DynElephant *x)
{
	if (munmap(x->addr, x->len) != 0)
		elog(ERROR, "munmap failed: %m");
}

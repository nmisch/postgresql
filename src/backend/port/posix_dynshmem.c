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

/* Maximum number of dynshmem segments.  TODO make it configurable. */
#define MAXDSM 1000

/*
 * Control data for dynamic shared memory, itself stored in permanent shared
 * memory.
 */
typedef struct DynShmemControl
{
	int size;				/* size of keys/refcnts arrays */

	/*
	 * Pseudorandom keys used to form POSIX shm paths.  After incrementing the
	 * associated refcnt, you may read a slot without a lock until such time
	 * as you decrement the refcnt again.  While holding DynShmemInitLock, you
	 * may read or write any slot.  Each slot begins as zero.  When a
	 * particular slot is needed for the first time, the requesting backend
	 * initializes the slot.  (These rules are somewhat stricter than the code
	 * needs at the moment, but there wouldn't presently be much advantage
	 * from weakening them.)
	 *
	 * We could just use a incrementing integer, but that would make us
	 * vulnerable to denial of service: a hostile local user could predict the
	 * offset we'll check next and allocate the segments above that.  Maybe
	 * this is overkill.
	 */
	uint64 *keys;

	uint32 *refcnts;		/* bitmap of unused offsets */
	slock_t refcnt_lck;		/* acquire to read or modify refcnts */
} DynShmemControl;

DynShmemControl *dsmctl;

Size
DynShmemShmemSize(void)
{
	return sizeof(struct DSMCTL) +
		MAXDSM * (sizeof(*dsmctl->refcnts) + sizeof(*dsmctl->keys));
}

void
DynShmemShmemInit()
{
	Size sz = DynShmemShmemSize();
	bool found;

	dsmctl = ShmemInitStruct("POSIX DynShmem Ctl", sz, &found);

	if (!IsUnderPostmaster)
	{
		/* Initialize shared memory area */
		Assert(!found);

		memset(dsmctl, 0, sz);
		SpinLockInit(&dsm->refcnt_lck);
		dsmctl->size = MAXDSM;
		/* Variable-size fields sit at the end of the allocation. */
		dsmctl->refcnts = dsmctl + 1;
		dsmctl->keys = dsmctl->refcnts + MAXDSM;
		memset(dsmctl->refcnts, 0xFF, sz);
	}
	else
		Assert(found);
}

/*
 * Find a free slot, increment its refcnt, and return the offset.  Remember
 * this slot in the current resource owner

 *
 *				xxxx0111
 * where x's are unspecified bits.  The two's complement negative is formed
 * by inverting all the bits and adding one.  Inversion gives
 *				yyyy1000
 * where each y is the inverse of the corresponding x.	Incrementing gives
 *				yyyyyy10000
 * and then ANDing with the original value gives
 *				00000010000
 */
static int
find_free_slot(void)
{
	volatile DynShmemControl *ctl = dsmctl;
	int i;

	ResourceOwnerEnlargeDynShmems(CurrentResourceOwner);

	SpinLockAcquire(&ctl->refcnt_lck);
	for (i = 0; i < ctl->size; i++)
	{
		if (ctl->refcnt[i] == 0)
		{
			ctl->refcnt[i]++;
			break;
		}
	}
	SpinLockRelease(&ctl->refcnt_lck);

	if (i == ctl->size)
		elog(ERROR, "no free DynShmem slots");

	ResourceOwnerRememberDynShmem(CurrentResourceOwner, i);

	return i;
}

/*
 * PostgreSQL.<int pid>.<int64 timestamp>.<uint64 key>
 */
static void
segname(char *buf, uint64 key)
{
	static char buf[512];

	/*
	 * Nothing says pid_t can't be wider than an int, but we're only using
	 * this as extra defense against the possibility that two postmasters
	 * start in the same microsecond.
	 */
	sprintf(buf,
			"PostgreSQL.%d.%" INT64_FORMAT ".%" UINT64_FORMAT,
			(int) PostmasterPID,
			TimestampTzToIntegerTimestamp(PgStartTime),
			key);
}

static void
persist_dsmctl(void)
{
	/* TODO */
}


static int
get_seg(void)
{
	int			offset;
	int32		bit;

	offset = find_free_slot();

	/* If the slot is initialized, open the existing object. */
	if (dsmctl->keys[offset] != 0)
	{
		fd = shm_open(segname(dsmctl->keys[offset]), O_RDWR, 0);
		if (fd == -1)
			elog(ERROR, "shm_open failed: %m");

		return fd;
	}

	/* Otherwise, initialize the slot. */
	for (;;)
	{
		key = new_random_key();
		LWLockAcquire(DynShmemInitLock, LW_EXCLUSIVE);
		dsmctl->keys[offset] = new_random_key;
		persist_dsmctl();
		LWLockRelease(DynShmemInitLock);

		fd = shm_open(segname(dsmctl->keys[offset]),
					  O_RDWR | O_CREAT | O_EXCL, IPCProtection);
		if (fd != -1)
			return fd;

		if (errno != EEXIST)
			elog(ERROR, "shm_open failed: %m");

		/*
		 * We collided with an existing segment; generate a new key and try
		 * again.
		 */
	}
}

/*
 * 
 *
 * A POSIX SHM implementation must persist objects independent of the process
 * that created them, and it is permitted to persist that past reboots.  A
 * semi-recent examples is FreeBSD 6.4 (2008).
 *
 * If we crash after an shm_unlink() starts and before truncate_dsmctl()
 * completes, the next startup may will try unlink segments a second time.
 * This is usually harmless, but there is a vanishing possibility that another
 * postmaster running under the same user account manages to choose the same
 * key in the mean time.  XXX could we include something like the data
 * directory or postmaster PID in the key to prevent this?
 */
#if 0
int
dsm_startup()
{
	int max;
	int i;
	int reclaimed = 0;

	recover_dsmctl(&keys, &size);

	for (i = 0; i < size; i++)
	{
		/*
		 * Skip uninitialized entries.  The initialized entries usually form a
		 * contiguous block at the front of the list, but holes are possible
		 * when multiple backends were iniitalizing slots concurrently.
		 */
		if (keys[i] == 0)
			continue;

		segname(key);
		if (shm_unlink(name) == 0)
			reclaimed++;
		else if (errno != ENOENT)
			elog(WARNING, "shm_unlink failed: %m");
	}

	pfree(keys);

	/*
	 */
	truncate_dsmctl();

	return reclaimed;
#if 0
	if (reclaimed > 0)
		elog(DEBUG1, "reclaimed %d shared memory segments", );
#endif
}
#endif

/*
 * Called at postmaster shutdown.  Unlink all segments, which should be on the
 * freelist and have zero length.  Then unlink the state file.
 */
void
DynShmemShutdown(void)
{
	int64	   *key, *end;

	/*
	 * No DynShmemInitLock acquisition; all ordinary backends should be gone
	 * already, so nobody else should be manipulating dynamic shared memory.
	 */
	for (key = dsmctl->keys, end = key + dsmctl->size; key < end; key++)
	{
		if (*key == 0)			/* See comment in DynShmemStartup(). */
			continue;

		/* TODO verify the freelist and size assumptions under
		   USE_ASSERT_CHECKING */

		if (shm_unlink(segname(*key)))
			elog(WARNING, "shm_unlink failed: %m");
	}
}

/*
 * Allocate a new shared memory segment of the specified length and attach it
 * to the current process at a system-chosen address.
 */
void
DynShmemCreate(DynShmem *x, Size len, int flags)
{
	int fd;

	/* Only supported case for the moment. */
	Assert(flags == DYNSHMEM_RESOWNER);

	x->local_attached = false;
	fd = get_seg();

	if (ftruncate(fd, len) != 0)
		elog(ERROR, "ftruncate failed: %m");
	x->len = len;

	x->addr = mmap(NULL, len,
				   PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_HASSEMAPHORE,
				   fd, 0);
	if (x->addr == MAP_FAILED)
		elog(ERROR, "mmap failed: %m");
	x->attached = true;

	/*
	 * XXX if we fail here, we need to reclaim the segment.  Probably need to
	 * associate segments with resource owners?  
	 */
	if (close(fd) != 0)
		elog(ERROR, "close failed: %m");

}

/*
 * Attempt to attach a dynamic shared memory area to the current process.
 * This is mostly for processes other than the creating process; though it's
 * permitted for the creating process to detach and later re-attach.
 */
void
DynShmemAttach(DynShmem *x)
{
	int fd;
	void *addr;

	fd = shm_open(segname(x->key), O_RDWR, 0);
	if (fd == -1)
		elog(ERROR, "shm_open failed: %m");

	addr = mmap(x->addr, x->len,
				PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_HASSEMAPHORE,
				fd, 0);
	if (addr == MAP_FAILED)
		elog(ERROR, "mmap failed: %m");
	if (addr != x->addr)
		elog(ERROR, "DynShmemAttach: address mismatch");

	if (close(fd) != 0)
		elog(ERROR, "close failed: %m");
}

/*
 * Undo an earlier DynShmemAttach() or DynShmemCreate() in this process.  This
 * frees the address space.  If we're the last process to detach, this also
 * frees the underlying allocation.
 */
void
DynShmemDetach(DynShmem *x)
{
	Assert(x->local_attached);

	if (munmap(x->addr, x->len) != 0)
		elog(ERROR, "munmap failed: %m");
	x->attached = false;
}

/*
 * Call this after every attached process has called DynShmemDetach().  This
 * will free all associated resources.

Call this after every attached process has ca
 * Only the owning process should call after detaching itself.  After this is
 * done, later attempts to attach to the same segment will fail.  It's
 * unspecified
 */
void
DynShmemDestroy(DynShmem *x)
{
	int fd;

	if (x->addr != NULL)
		

	dsm_freelist_put();

	fd = shm_open(segname(x->key), O_RDWR, 0);
	if (fd == -1)
		elog(ERROR, "shm_open failed: %m");
}


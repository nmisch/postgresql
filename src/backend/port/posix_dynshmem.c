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

/* PostgreSQL.<10-char 31-bit port>.<10-char 31-bit ordinal> */
#define MAX_SHM_NAME 64

typedef struct DSMPSM
{
	slock_t freelist_lck;		/* protects all other fields */
	uint32 freelist[16];		/* bitmap of unused offsets */
	uint64 keys[512];
} DSPSM;

struct DSMPSM *dsmpsm;

Size
DynShmemShmemSize()
{
	return sizeof(struct DSMPSM);
}

void
DynShmemShmemInit()
{
	Size sz = DynShmemShmemSize();
	bool found;

	dsmpsm = ShmemInitStruct("POSIX DynShmem Status", sz, &found);

	if (!IsUnderPostmaster)
	{
		/* Initialize shared memory area */
		Assert(!found);

		MemSet(dsmpsm, 0, sz);
	}
	else
		Assert(found);
}

/*
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
allocate_unset_bit()
{
	/* Find the first free entry. */
	for (wordnum = 0; wordnum < nwords; ++wordnum)
	{
		int32		w = dspsm->freelist[wordnum];


		if (w != 0xFFFFFFFF)
		{
			
		}
	}


, and inspect its cell.  If the cell has valid content that object
object exists.  Release the spinlock and ftruncate to the correct length.

}

/*
 * Buffer must have MAXNAME worth of space.
 */
static void
segname(char *buf, uint64 key)
{
	sprintf(buf, "PostgreSQL.%lu", key);
}


int
get_seg(void)
{
	int32	   *word = dsmpsm->freelist;
	int32		bit;

	SpinLockAcquire(dsmpsm->freelist_lck);
	offset = allocate_unset_bit();
	SpinLockRelease(dsmpsm->freelist_lck);

	/* If the slot is initialized, open the existing object. */
	if (dsmpsm->keys[offset] != 0)
	{
		segname(namebuf, dsmpsm->keys[offset]);
		fd = shm_open(name, O_RDWR, IPCProtection);
		if (fd == -1)
			elog(ERROR, "shm_open failed: %m");

		return fd;
	}

	/*
	 * If the offset is uninitialized, initialize it.  We proect this with a
	 * lock so another backend so concurrent
	 */
	for (;;)
	{
		key = new_random_key();
		LWLockAcquire(PSMLock, LW_EXCLUSIVE);
		dsmpsm->keys[offset] = new_random_key;
		persist_dsmpsm();
		LWLockRelease(PSMLock);

		segname(namebuf, dsmpsm->keys[offset]);
		fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, IPCProtection);
		if (fd != -1)
			return fd;

		/*
		 * If we collided with an existing segment, generate a new key and try
		 * again.
		 */
		if (errno != EEXIST)
			elog(ERROR, "shm_open failed: %m");
	}
}

/*
 * 
 *
 * A POSIX SHM implementation must persist objects independent of the process
 * that created them, and it is permitted to persist that past reboots.  A
 * semi-recent examples is FreeBSD 6.4 (2008).
 *
 * If we crash after an shm_unlink() starts and before truncate_dsmpsm()
 * completes, the next startup may will try unlink segments a second time.
 * This is usually harmless, but there is a vanishing possibility that another
 * postmaster running under the same user account manages to choose the same
 * key in the mean time.  XXX could we include something like the data
 * directory or postmaster PID in the key to prevent this?
 */
int
dsm_startup()
{
	int max;
	int i;
	int reclaimed = 0;

	recover_dsmpsm(&keys, &size);

	for (i = 0; i < size; i++)
	{
		/*
		 * Skip uninitialized entries.  The initialized entries usually form a
		 * contiguous block at the front of the list, but holes are possible
		 * when multiple backends were iniitalizing slots concurrently.
		 */
		if (keys[i] == 0)
			continue;

		segname(namebuf, key);
		if (shm_unlink(name) == 0)
			reclaimed++;
		else if (errno != ENOENT)
			elog(WARNING, "shm_unlink failed: %m");
	}

	pfree(keys);

	/*
	 */
	truncate_dsmpsm();

	return reclaimed;
#if 0
	if (reclaimed > 0)
		elog(DEBUG1, "reclaimed %d shared memory segments", );
#endif
}

/*
 * Allocate a new shared memory segment of the specified length and attach it
 * to the current process at a system-chosen address.
 */
void
dsm_create(DynElephant *x, Size len)
{
	int fd;
	char *name[MAX_SHM_NAME];

	segname(name, -1);			/* reserve a new name */

	/*
	 * We presume startup destroyed any lingering memory objects, so we use
	 * O_EXCL to detect unexpected reuse.
	 */
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, IPCProtection);
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

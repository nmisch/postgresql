/*-------------------------------------------------------------------------
 *
 * pg_shmem.h
 *	  Platform-independent API for shared memory support.
 *
 * Every port is expected to support shared memory with approximately
 * SysV-ish semantics; in particular, a memory block is not anonymous
 * but has an ID, and we must be able to tell whether there are any
 * remaining processes attached to a block of a specified ID.
 *
 * To simplify life for the SysV implementation, the ID is assumed to
 * consist of two unsigned long values (these are key and ID in SysV
 * terms).	Other platforms may ignore the second value if they need
 * only one ID number.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/pg_shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SHMEM_H
#define PG_SHMEM_H

typedef struct PGShmemHeader	/* standard header for all Postgres shmem */
{
	int32		magic;			/* magic # to identify Postgres segments */
#define PGShmemMagic  679834894
	pid_t		creatorPID;		/* PID of creating process */
	Size		totalsize;		/* total size of segment */
	Size		freeoffset;		/* offset to first free space */
	void	   *index;			/* pointer to ShmemIndex table */
#ifndef WIN32					/* Windows doesn't have useful inode#s */
	dev_t		device;			/* device data directory is on */
	ino_t		inode;			/* inode number of data directory */
#endif
} PGShmemHeader;


#ifdef EXEC_BACKEND
#ifndef WIN32
extern unsigned long UsedShmemSegID;
#else
extern HANDLE UsedShmemSegID;
#endif
extern void *UsedShmemSegAddr;

extern void PGSharedMemoryReAttach(void);
#endif

/* Definitions needed by multiple implementations. */
#ifndef WIN32

#define IPCProtection	(0600)	/* access/modify by user only */

#ifdef SHM_SHARE_MMU			/* use intimate shared memory on Solaris */
#define PG_SHMAT_FLAGS			SHM_SHARE_MMU
#else
#define PG_SHMAT_FLAGS			0
#endif

/* Linux prefers MAP_ANONYMOUS, but the flag is called MAP_ANON on other systems. */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS			MAP_ANON
#endif

/* BSD-derived systems have MAP_HASSEMAPHORE, but it's not present (or needed) on Linux. */
#ifndef MAP_HASSEMAPHORE
#define MAP_HASSEMAPHORE		0
#endif

#define PG_MMAP_FLAGS			(MAP_SHARED|MAP_ANONYMOUS|MAP_HASSEMAPHORE)

/* Some really old systems don't define MAP_FAILED. */
#ifndef MAP_FAILED
#define MAP_FAILED ((void *) -1)
#endif

#endif

/* pg_shmem.c - permanent shared memory available to all backends */
extern PGShmemHeader *PGSharedMemoryCreate(Size size, bool makePrivate,
					 int port);
extern bool PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2);
extern void PGSharedMemoryDetach(void);

/* pg_dynshmem.c - on-demand shared memory */
typedef struct DynShmem
{
	int64	key;				/* distinguish shm areas of this postmaster */
	void   *addr;				/* where mapped, or where to map) */
	Size	len;				/* allocation length */
	bool	local_attached;		/* current process has mapped at addr */
} DynShmem;

/*** Flags for DynShmemCreate() ***/
/*
 * You must specify one of DYNSHMEM_RESOWNER or DYNSHMEM_PERMANENT.
 * DYNSHMEM_RESOWNER ties the segment to the current resource owner;
 * (sub)transaction abort will automatically detach it, and transaction commit
 * will complain and detach it if that is not already done.
 * DYNSHMEM_PERMANENT attachments last until the process exits or until
 * explicitly detached.
 */
#define DYNSHMEM_RESOWNER				0x0001
#define DYNSHMEM_PERMANENT				0x0002
/*
 * Flag indicating that the content of the region is position-independent.
 * The system may forgo efforts to place the memory at the same address in
 * every process.
 */
#define DYNSHMEM_POSITION_INDEPENDENT	0x0004
/*
 * Flag indicating that post-fork() attachments should be possible.  When
 * DYNSHMEM_POSITION_INDEPENDENT is also specified, this allocation will draw
 * from a reserved pool of address space to ensure that every process can map
 * at the same address.  The request will fail if that pool lacks sufficient
 * free space.
 */
#define DYNSHMEM_POST_FORK				0x0008

extern void dsm_create(DynShmem *x, Size len);
extern void dsm_destroy(DynShmem *x);
extern void dsm_attach(DynShmem *x);
extern void dsm_detach(DynShmem *x);

#endif   /* PG_SHMEM_H */

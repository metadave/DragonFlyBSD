/*
 * Copyright (c) 2011-2012 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@dragonflybsd.org>
 * by Venkatesh Srinivas <vsrinivas@dragonflybsd.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This header file contains structures used internally by the HAMMER2
 * implementation.  See hammer2_disk.h for on-disk structures.
 */

#ifndef _VFS_HAMMER2_HAMMER2_H_
#define _VFS_HAMMER2_HAMMER2_H_

#include <sys/param.h>
#include <sys/types.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/tree.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/proc.h>
#include <sys/mountctl.h>
#include <sys/priv.h>
#include <sys/stat.h>
#include <sys/globaldata.h>
#include <sys/lockf.h>
#include <sys/buf.h>
#include <sys/queue.h>
#include <sys/limits.h>
#include <sys/buf2.h>
#include <sys/signal2.h>
#include <sys/tree.h>

#include "hammer2_disk.h"
#include "hammer2_mount.h"

struct hammer2_inode;
struct hammer2_mount;

struct hammer2_node;

/*
 * A hammer2 inode.
 */
struct hammer2_inode {
	struct hammer2_mount	*mp;
	struct lock		lk;
	struct vnode		*vp;
	hammer2_tid_t		inum;
	unsigned char		type;
};

#define HAMMER2_INODE_TYPE_DIR	0x01
#define HAMMER2_INODE_TYPE_FILE	0x02
#define HAMMER2_INODE_TYPE_ROOT	0x10
#define HAMMER2_INODE_TYPE_MASK	0x07

/* --------------------------------------------------------------------- */

/*
 * Internal representation of a hammer2 directory entry.
 */
struct hammer2_dirent {
	TAILQ_ENTRY(hammer2_dirent)	td_entries;

	/* Length of the name stored in this directory entry.  This avoids
	 * the need to recalculate it every time the name is used. */
	uint16_t			td_namelen;

	/* The name of the entry, allocated from a string pool.  This
	 * string is not required to be zero-terminated; therefore, the
	 * td_namelen field must always be used when accessing its value. */
	char *				td_name;

	/* Pointer to the node this entry refers to. */
	struct hammer2_node *		td_node;
};

/* A directory in hammer2 holds a sorted list of directory entries, which in
 * turn point to other files (which can be directories themselves).
 *
 * In hammer2, this list is managed by a tail queue, whose head is defined by
 * the struct hammer2_dir type.
 *
 * It is imporant to notice that directories do not have entries for . and
 * .. as other file systems do.  These can be generated when requested
 * based on information available by other means, such as the pointer to
 * the node itself in the former case or the pointer to the parent directory
 * in the latter case.  This is done to simplify hammer2's code and, more
 * importantly, to remove redundancy. */
TAILQ_HEAD(hammer2_dir, hammer2_dirent);

/* --------------------------------------------------------------------- */



/* --------------------------------------------------------------------- */

/*
 * Internal representation of a hammer2 file system node.
 *
 * This structure is splitted in two parts: one holds attributes common
 * to all file types and the other holds data that is only applicable to
 * a particular type.  The code must be careful to only access those
 * attributes that are actually allowed by the node's type.
 *
 *
 * Below is the key of locks used to protected the fields in the following
 * structures.
 *
 */
struct hammer2_node {
	/* Doubly-linked list entry which links all existing nodes for a
	 * single file system.  This is provided to ease the removal of
	 * all nodes during the unmount operation. */
	LIST_ENTRY(tmpfs_node)	tn_entries;

	/* The node's type.  Any of 'VBLK', 'VCHR', 'VDIR', 'VFIFO',
	 * 'VLNK', 'VREG' and 'VSOCK' is allowed.  The usage of vnode
	 * types instead of a custom enumeration is to make things simpler
	 * and faster, as we do not need to convert between two types. */
	enum vtype		tn_type;

	/* Node identifier. */
	ino_t			tn_id;

	/* Node's internal status.  This is used by several file system
	 * operations to do modifications to the node in a delayed
	 * fashion. */
	int			tn_status;
#define	TMPFS_NODE_ACCESSED	(1 << 1)
#define	TMPFS_NODE_MODIFIED	(1 << 2)
#define	TMPFS_NODE_CHANGED	(1 << 3)

	/* The node size.  It does not necessarily match the real amount
	 * of memory consumed by it. */
	off_t			tn_size;

	/* Generic node attributes. */
	uid_t			tn_uid;
	gid_t			tn_gid;
	mode_t			tn_mode;
	int			tn_flags;
	nlink_t			tn_links;
	int32_t			tn_atime;
	int32_t			tn_atimensec;
	int32_t			tn_mtime;
	int32_t			tn_mtimensec;
	int32_t			tn_ctime;
	int32_t			tn_ctimensec;
	unsigned long		tn_gen;
	struct lockf		tn_advlock;

	/* As there is a single vnode for each active file within the
	 * system, care has to be taken to avoid allocating more than one
	 * vnode per file.  In order to do this, a bidirectional association
	 * is kept between vnodes and nodes.
	 *
	 * Whenever a vnode is allocated, its v_data field is updated to
	 * point to the node it references.  At the same time, the node's
	 * tn_vnode field is modified to point to the new vnode representing
	 * it.  Further attempts to allocate a vnode for this same node will
	 * result in returning a new reference to the value stored in
	 * tn_vnode.
	 *
	 * May be NULL when the node is unused (that is, no vnode has been
	 * allocated for it or it has been reclaimed). */
	struct vnode *		tn_vnode;

	/* interlock to protect tn_vpstate */
	struct lock		tn_interlock;

	/* Identify if current node has vnode assiocate with
	 * or allocating vnode.
	 */
	int		tn_vpstate;

	/* misc data field for different tn_type node */
	union {
		/* Valid when tn_type == VBLK || tn_type == VCHR. */
		dev_t			tn_rdev; /*int32_t ?*/

		/* Valid when tn_type == VDIR. */
		struct tn_dir{
			/* Pointer to the parent directory.  The root
			 * directory has a pointer to itself in this field;
			 * this property identifies the root node. */
			struct tmpfs_node *	tn_parent;

			/* Head of a tail-queue that links the contents of
			 * the directory together.  See above for a
			 * description of its contents. */
			struct tmpfs_dir	tn_dirhead;

			/* Number and pointer of the first directory entry
			 * returned by the readdir operation if it were
			 * called again to continue reading data from the
			 * same directory as before.  This is used to speed
			 * up reads of long directories, assuming that no
			 * more than one read is in progress at a given time.
			 * Otherwise, these values are discarded and a linear
			 * scan is performed from the beginning up to the
			 * point where readdir starts returning values. */
			off_t			tn_readdir_lastn;
			struct tmpfs_dirent *	tn_readdir_lastp;
		}tn_dir;

		/* Valid when tn_type == VLNK. */
		/* The link's target, allocated from a string pool. */
		char *			tn_link;

		/* Valid when tn_type == VREG. */
		struct tn_reg {
			/* The contents of regular files stored in a tmpfs
			 * file system are represented by a single anonymous
			 * memory object (aobj, for short).  The aobj provides
			 * direct access to any position within the file,
			 * because its contents are always mapped in a
			 * contiguous region of virtual memory.  It is a task
			 * of the memory management subsystem (see uvm(9)) to
			 * issue the required page ins or page outs whenever
			 * a position within the file is accessed. */
			vm_object_t		tn_aobj;
			size_t			tn_aobj_pages;

		}tn_reg;

		/* Valid when tn_type = VFIFO */
		struct tn_fifo {
			int (*tn_fo_read)  (struct file *fp, struct uio *uio,
					    struct ucred *cred, int flags);
			int (*tn_fo_write) (struct file *fp, struct uio *uio,
					    struct ucred *cred, int flags);
		}tn_fifo;
	}tn_spec;
};
LIST_HEAD(tmpfs_node_list, tmpfs_node);

#define tn_rdev tn_spec.tn_rdev
#define tn_dir tn_spec.tn_dir
#define tn_link tn_spec.tn_link
#define tn_reg tn_spec.tn_reg
#define tn_fifo tn_spec.tn_fifo

#define TMPFS_NODE_LOCK(node) lockmgr(&(node)->tn_interlock, LK_EXCLUSIVE|LK_RETRY)
#define TMPFS_NODE_UNLOCK(node) lockmgr(&(node)->tn_interlock, LK_RELEASE)
#define TMPFS_NODE_MTX(node) (&(node)->tn_interlock)

#ifdef INVARIANTS
#define TMPFS_ASSERT_LOCKED(node) do {					\
KKASSERT(node != NULL);					\
KKASSERT(node->tn_vnode != NULL);			\
if (!vn_islocked(node->tn_vnode) &&			\
(lockstatus(TMPFS_NODE_MTX(node), curthread) == LK_EXCLUSIVE ))		\
panic("tmpfs: node is not locked: %p", node);	\
} while (0)
#define TMPFS_ASSERT_ELOCKED(node) do {					\
KKASSERT((node) != NULL);				\
KKASSERT(lockstatus(TMPFS_NODE_MTX(node), curthread) == LK_EXCLUSIVE);		\
} while (0)
#else
#define TMPFS_ASSERT_LOCKED(node) (void)0
#define TMPFS_ASSERT_ELOCKED(node) (void)0
#endif

#define TMPFS_VNODE_ALLOCATING	1
#define TMPFS_VNODE_WANT	2
#define TMPFS_VNODE_DOOMED	4
/* --------------------------------------------------------------------- */



/*
 * Governing mount structure for filesystem (aka vp->v_mount)
 */
struct hammer2_mount {
	struct mount	*hm_mp;
	int		hm_ronly;	/* block device mounted read-only */
	struct vnode	*hm_devvp;	/* device vnode */
	struct lock	hm_lk;

	/* Root inode */
	struct hammer2_inode	*hm_iroot;

	/* Per-mount inode zone */
	struct malloc_type *hm_inodes;
	int 		hm_ninodes;
	int 		hm_maxinodes;

	struct malloc_type *hm_ipstacks;
	int		hm_nipstacks;
	int		hm_maxipstacks;

	struct hammer2_volume_data hm_sb;


	/*** TMPFS_MOUNT ***/


	/* Maximum number of memory pages available for use by the file
	 * system, set during mount time.  This variable must never be
	 * used directly as it may be bigger than the current amount of
	 * free memory; in the extreme case, it will hold the SIZE_MAX
	 * value.  Instead, use the TMPFS_PAGES_MAX macro. */
	vm_pindex_t		tm_pages_max;

	/* Number of pages in use by the file system.  Cannot be bigger
	 * than the value returned by TMPFS_PAGES_MAX in any case. */
	vm_pindex_t		tm_pages_used;

	/* Pointer to the node representing the root directory of this
	 * file system. */
	struct tmpfs_node *	tm_root;

	/* Maximum number of possible nodes for this file system; set
	 * during mount time.  We need a hard limit on the maximum number
	 * of nodes to avoid allocating too much of them; their objects
	 * cannot be released until the file system is unmounted.
	 * Otherwise, we could easily run out of memory by creating lots
	 * of empty files and then simply removing them. */
	ino_t			tm_nodes_max;

	/* Number of nodes currently that are in use. */
	ino_t			tm_nodes_inuse;

	/* maximum representable file size */
	u_int64_t		tm_maxfilesize;

	/* Nodes are organized in two different lists.  The used list
	 * contains all nodes that are currently used by the file system;
	 * i.e., they refer to existing files.  The available list contains
	 * all nodes that are currently available for use by new files.
	 * Nodes must be kept in this list (instead of deleting them)
	 * because we need to keep track of their generation number (tn_gen
	 * field).
	 *
	 * Note that nodes are lazily allocated: if the available list is
	 * empty and we have enough space to create more nodes, they will be
	 * created and inserted in the used list.  Once these are released,
	 * they will go into the available list, remaining alive until the
	 * file system is unmounted. */
	struct tmpfs_node_list	tm_nodes_used;

	/* All node lock to protect the node list and tmp_pages_used */
	struct lock		 allnode_lock;

	/* Per-mount malloc zones for tmpfs nodes, names, and dirents */
	struct malloc_type	*tm_node_zone;
	struct malloc_type	*tm_dirent_zone;
	struct malloc_type	*tm_name_zone;

	struct objcache_malloc_args tm_node_zone_malloc_args;
	struct objcache_malloc_args tm_dirent_zone_malloc_args;

	/* Pools used to store file system meta data.  These are not shared
	 * across several instances of tmpfs for the reasons described in
	 * tmpfs_pool.c. */
	struct objcache		*tm_dirent_pool;
	struct objcache		*tm_node_pool;

	int			tm_flags;
};

#if defined(_KERNEL)

MALLOC_DECLARE(M_HAMMER2);


static inline struct mount *
H2TOMP(struct hammer2_mount *hmp)
{
	return (struct mount *) hmp->hm_mp;
}

#define VTOI(vp)	((struct hammer2_inode *) (vp)->v_data)
#define ITOV(ip)	((ip)->vp)

extern struct vop_ops hammer2_vnode_vops;
extern struct vop_ops hammer2_spec_vops;
extern struct vop_ops hammer2_fifo_vops;

/* hammer2_inode.c */

extern int hammer2_inactive(struct vop_inactive_args *);
extern int hammer2_reclaim(struct vop_reclaim_args *);

/* hammer2_subr.c */

extern struct vnode *igetv(struct hammer2_inode *, int *);

extern void hammer2_mount_exlock(struct hammer2_mount *);
extern void hammer2_mount_shlock(struct hammer2_mount *);
extern void hammer2_mount_unlock(struct hammer2_mount *);



#endif	/* kernel */
#endif

#ifndef _VFS_TMPFS_TMPFS_H_
#define _VFS_TMPFS_TMPFS_H_

/* ---------------------------------------------------------------------
 * KERNEL-SPECIFIC DEFINITIONS
 * --------------------------------------------------------------------- */
#include <sys/dirent.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/vnode.h>
#include <sys/file.h>
#include <sys/lock.h>
#include <sys/lockf.h>
#include <sys/mutex.h>
#include <sys/objcache.h>

/* --------------------------------------------------------------------- */
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/vmmeter.h>
#include <vm/swap_pager.h>

MALLOC_DECLARE(M_TMPFSMNT);

/* Each entry in a directory has a cookie that identifies it.  Cookies
 * supersede offsets within directories because, given how tmpfs stores
 * directories in memory, there is no such thing as an offset.  (Emulating
 * a real offset could be very difficult.)
 *
 * The '.', '..' and the end of directory markers have fixed cookies which
 * cannot collide with the cookies generated by other entries.  The cookies
 * for the other entries are generated based on the memory address on which
 * stores their information is stored.
 *
 * Ideally, using the entry's memory pointer as the cookie would be enough
 * to represent it and it wouldn't cause collisions in any system.
 * Unfortunately, this results in "offsets" with very large values which
 * later raise problems in the Linux compatibility layer (and maybe in other
 * places) as described in PR kern/32034.  Hence we need to workaround this
 * with a rather ugly hack.
 *
 * Linux 32-bit binaries, unless built with _FILE_OFFSET_BITS=64, have off_t
 * set to 'long', which is a 32-bit *signed* long integer.  Regardless of
 * the macro value, GLIBC (2.3 at least) always uses the getdents64
 * system call (when calling readdir) which internally returns off64_t
 * offsets.  In order to make 32-bit binaries work, *GLIBC* converts the
 * 64-bit values returned by the kernel to 32-bit ones and aborts with
 * EOVERFLOW if the conversion results in values that won't fit in 32-bit
 * integers (which it assumes is because the directory is extremely large).
 * This wouldn't cause problems if we were dealing with unsigned integers,
 * but as we have signed integers, this check fails due to sign expansion.
 *
 * For example, consider that the kernel returns the 0xc1234567 cookie to
 * userspace in a off64_t integer.  Later on, GLIBC casts this value to
 * off_t (remember, signed) with code similar to:
 *     system call returns the offset in kernel_value;
 *     off_t casted_value = kernel_value;
 *     if (sizeof(off_t) != sizeof(off64_t) &&
 *         kernel_value != casted_value)
 *             error!
 * In this case, casted_value still has 0xc1234567, but when it is compared
 * for equality against kernel_value, it is promoted to a 64-bit integer and
 * becomes 0xffffffffc1234567, which is different than 0x00000000c1234567.
 * Then, GLIBC assumes this is because the directory is very large.
 *
 * Given that all the above happens in user-space, we have no control over
 * it; therefore we must workaround the issue here.  We do this by
 * truncating the pointer value to a 32-bit integer and hope that there
 * won't be collisions.  In fact, this will not cause any problems in
 * 32-bit platforms but some might arise in 64-bit machines (I'm not sure
 * if they can happen at all in practice).
 *
 * XXX A nicer solution shall be attempted. */
#ifdef _KERNEL
#define	TMPFS_DIRCOOKIE_DOT	0
#define	TMPFS_DIRCOOKIE_DOTDOT	1
#define	TMPFS_DIRCOOKIE_EOF	2
static __inline
off_t
tmpfs_dircookie(struct tmpfs_dirent *de)
{
	off_t cookie;

	cookie = ((off_t)(uintptr_t)de >> 1) & 0x7FFFFFFF;
	KKASSERT(cookie != TMPFS_DIRCOOKIE_DOT);
	KKASSERT(cookie != TMPFS_DIRCOOKIE_DOTDOT);
	KKASSERT(cookie != TMPFS_DIRCOOKIE_EOF);

	return cookie;
}
#endif


/*
 * Internal representation of a tmpfs mount point.
 */

#define TMPFS_LOCK(tm) lockmgr(&(tm)->allnode_lock, LK_EXCLUSIVE|LK_RETRY)
#define TMPFS_UNLOCK(tm) lockmgr(&(tm)->allnode_lock, LK_RELEASE)

/* --------------------------------------------------------------------- */

/*
 * This structure maps a file identifier to a tmpfs node.  Used by the
 * NFS code.
 */
struct tmpfs_fid {
	uint16_t		tf_len;
	uint16_t		tf_pad;
	ino_t			tf_id;
	unsigned long		tf_gen;
};

/* --------------------------------------------------------------------- */

#ifdef _KERNEL
/*
 * Prototypes for tmpfs_subr.c.
 */

int	tmpfs_alloc_node(struct hammer2_mount *, enum vtype,
			 uid_t uid, gid_t gid, mode_t mode, struct tmpfs_node *,
			 char *, int, int, struct tmpfs_node **);
void	tmpfs_free_node(struct hammer2_mount *, struct tmpfs_node *);
int	tmpfs_alloc_dirent(struct hammer2_mount *, struct tmpfs_node *,
			   const char *, uint16_t, struct tmpfs_dirent **);
void	tmpfs_free_dirent(struct hammer2_mount *, struct tmpfs_dirent *);
int	tmpfs_alloc_vp(struct mount *, struct tmpfs_node *, int,
		       struct vnode **);
void	tmpfs_free_vp(struct vnode *);
int	tmpfs_alloc_file(struct vnode *, struct vnode **, struct vattr *,
			 struct namecache *, struct ucred *, char *);
void	tmpfs_dir_attach(struct tmpfs_node *, struct tmpfs_dirent *);
void	tmpfs_dir_detach(struct tmpfs_node *, struct tmpfs_dirent *);
struct tmpfs_dirent *	tmpfs_dir_lookup(struct tmpfs_node *node,
					 struct tmpfs_node *f,
					 struct namecache *ncp);
int	tmpfs_dir_getdotdent(struct tmpfs_node *, struct uio *);
int	tmpfs_dir_getdotdotdent(struct hammer2_mount *,
				struct tmpfs_node *, struct uio *);
struct tmpfs_dirent *	tmpfs_dir_lookupbycookie(struct tmpfs_node *, off_t);
int	tmpfs_dir_getdents(struct tmpfs_node *, struct uio *, off_t *);
int	tmpfs_reg_resize(struct vnode *, off_t, int);
int	tmpfs_chflags(struct vnode *, int, struct ucred *);
int	tmpfs_chmod(struct vnode *, mode_t, struct ucred *);
int	tmpfs_chown(struct vnode *, uid_t, gid_t, struct ucred *);
int	tmpfs_chsize(struct vnode *, u_quad_t, struct ucred *);
int	tmpfs_chtimes(struct vnode *, struct timespec *, struct timespec *,
		      int, struct ucred *);
void	tmpfs_itimes(struct vnode *, const struct timespec *,
		     const struct timespec *);

void	tmpfs_update(struct vnode *);
int	tmpfs_truncate(struct vnode *, off_t);
int	tmpfs_node_ctor(void *obj, void *privdata, int flags);

/* --------------------------------------------------------------------- */

/*
 * Convenience macros to simplify some logical expressions.
 */
#define IMPLIES(a, b) (!(a) || (b))
#define IFF(a, b) (IMPLIES(a, b) && IMPLIES(b, a))

/* --------------------------------------------------------------------- */

/*
 * Checks that the directory entry pointed by 'de' matches the name 'name'
 * with a length of 'len'.
 */
#define TMPFS_DIRENT_MATCHES(de, name, len) \
(de->td_namelen == (uint16_t)len && \
bcmp((de)->td_name, (name), (de)->td_namelen) == 0)

/* --------------------------------------------------------------------- */

/*
 * Ensures that the node pointed by 'node' is a directory and that its
 * contents are consistent with respect to directories.
 */
#define TMPFS_VALIDATE_DIR(node) \
KKASSERT((node)->tn_type == VDIR); \
KKASSERT((node)->tn_size % sizeof(struct tmpfs_dirent) == 0); \
KKASSERT((node)->tn_dir.tn_readdir_lastp == NULL || \
tmpfs_dircookie((node)->tn_dir.tn_readdir_lastp) == (node)->tn_dir.tn_readdir_lastn);

#endif

/* --------------------------------------------------------------------- */

/*
 * Macros/functions to convert from generic data structures to tmpfs
 * specific ones.
 */

static inline
struct hammer2_mount *
VFS_TO_TMPFS(struct mount *mp)
{
	struct hammer2_mount *tmp;

	KKASSERT((mp) != NULL && (mp)->mnt_data != NULL);
	tmp = (struct hammer2_mount *)(mp)->mnt_data;
	return tmp;
}

static inline
struct tmpfs_node *
VP_TO_TMPFS_NODE(struct vnode *vp)
{
	struct tmpfs_node *node;

	KKASSERT((vp) != NULL && (vp)->v_data != NULL);
	node = (struct tmpfs_node *)vp->v_data;
	return node;
}

static inline
struct tmpfs_node *
VP_TO_TMPFS_DIR(struct vnode *vp)
{
	struct tmpfs_node *node;

	node = VP_TO_TMPFS_NODE(vp);
	TMPFS_VALIDATE_DIR(node);
	return node;
}

/* --------------------------------------------------------------------- */
/*
 * buffer cache size
 */
#define BSIZE (off_t)16384          /* buffer cache size*/
#define BMASK (off_t)(BSIZE - 1)

extern struct vop_ops tmpfs_vnode_vops;
extern struct vop_ops tmpfs_fifo_vops;

/*
 * Declarations for tmpfs_vnops.c.
 */

int tmpfs_access(struct vop_access_args *);
int tmpfs_getattr(struct vop_getattr_args *);
int tmpfs_setattr(struct vop_setattr_args *);
int tmpfs_reclaim(struct vop_reclaim_args *);


#endif /* _VFS_TMPFS_TMPFS_H_ */

#ifndef _MORE_HAMMER2_
#define _MORE_HAMMER2_
static inline struct hammer2_mount *
MPTOH2(struct mount *mp)
{
	return (struct hammer2_mount *) mp->mnt_data;
}
#endif
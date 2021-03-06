/*
 * Copyright (c) 2011-2013 The DragonFly Project.  All rights reserved.
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

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/lock.h>
#include <sys/uuid.h>

#include "hammer2.h"

#define FLUSH_DEBUG 0

/*
 * Recursively flush the specified chain.  The chain is locked and
 * referenced by the caller and will remain so on return.  The chain
 * will remain referenced throughout but can temporarily lose its
 * lock during the recursion to avoid unnecessarily stalling user
 * processes.
 */
struct hammer2_flush_info {
	hammer2_chain_t *parent;
	hammer2_trans_t	*trans;
	int		depth;
	int		diddeferral;
	int		pass;
	int		cache_index;
	int		domodify;
	struct h2_flush_deferral_list flush_list;
	hammer2_tid_t	sync_tid;	/* flush synchronization point */
};

typedef struct hammer2_flush_info hammer2_flush_info_t;

static void hammer2_chain_flush_core(hammer2_flush_info_t *info,
				hammer2_chain_t **chainp);
static int hammer2_chain_flush_scan1(hammer2_chain_t *child, void *data);
static int hammer2_chain_flush_scan2(hammer2_chain_t *child, void *data);
static void hammer2_rollup_stats(hammer2_chain_t *parent,
				hammer2_chain_t *child, int how);

/*
 * Can we ignore a chain for the purposes of flushing modifications
 * to the media?
 */
static __inline
int
h2ignore_deleted(hammer2_flush_info_t *info, hammer2_chain_t *chain)
{
	return (chain->delete_tid <= info->sync_tid &&
		(chain->bref.type != HAMMER2_BREF_TYPE_INODE ||
		 (chain->flags & HAMMER2_CHAIN_DESTROYED)));
}

#if 0
static __inline
void
hammer2_updatestats(hammer2_flush_info_t *info, hammer2_blockref_t *bref,
		    int how)
{
	hammer2_key_t bytes;

	if (bref->type != 0) {
		bytes = 1 << (bref->data_off & HAMMER2_OFF_MASK_RADIX);
		if (bref->type == HAMMER2_BREF_TYPE_INODE)
			info->inode_count += how;
		if (how < 0)
			info->data_count -= bytes;
		else
			info->data_count += bytes;
	}
}
#endif

/*
 * Transaction support functions for writing to the filesystem.
 *
 * Initializing a new transaction allocates a transaction ID.  Typically
 * passed a pmp (hmp passed as NULL), indicating a cluster transaction.  Can
 * be passed a NULL pmp and non-NULL hmp to indicate a transaction on a single
 * media target.  The latter mode is used by the recovery code.
 *
 * TWO TRANSACTION IDs can run concurrently, where one is a flush and the
 * other is a set of any number of concurrent filesystem operations.  We
 * can either have <running_fs_ops> + <waiting_flush> + <blocked_fs_ops>
 * or we can have <running_flush> + <concurrent_fs_ops>.
 *
 * During a flush, new fs_ops are only blocked until the fs_ops prior to
 * the flush complete.  The new fs_ops can then run concurrent with the flush.
 *
 * Buffer-cache transactions operate as fs_ops but never block.  A
 * buffer-cache flush will run either before or after the current pending
 * flush depending on its state.
 *
 * sync_tid vs real_tid.  For flush transactions ONLY, the flush operation
 * actually uses two transaction ids, one for the flush operation itself,
 * and <N+1> for any freemap allocations made as a side-effect.  real_tid
 * is fixed at <N>, sync_tid is adjusted dynamically as-needed.
 *
 * NOTE: The sync_tid for a flush's freemap allocation will match the
 *	 sync_tid of the following <concurrent_fs_ops> transaction(s).
 *	 The freemap topology will be out-of-step by one transaction id
 *	 in order to give the flusher a stable freemap topology to flush
 *	 out.  This is fixed up at mount-time using a quick incremental
 *	 scan.
 */
void
hammer2_trans_init(hammer2_trans_t *trans, hammer2_pfsmount_t *pmp,
		   hammer2_mount_t *hmp, int flags)
{
	hammer2_trans_t *head;

	bzero(trans, sizeof(*trans));
	if (pmp) {
		trans->pmp = pmp;
		KKASSERT(hmp == NULL);
		hmp = pmp->cluster.chains[0]->hmp;	/* XXX */
	} else {
		trans->hmp_single = hmp;
		KKASSERT(hmp);
	}

	hammer2_voldata_lock(hmp);
	trans->flags = flags;
	trans->td = curthread;
	/*trans->delete_gen = 0;*/	/* multiple deletions within trans */

	if (flags & HAMMER2_TRANS_ISFLUSH) {
		/*
		 * If multiple flushes are trying to run we have to
		 * wait until it is our turn.  All flushes are serialized.
		 *
		 * We queue ourselves and then wait to become the head
		 * of the queue, allowing all prior flushes to complete.
		 *
		 * A unique transaction id is required to avoid confusion
		 * when updating the block tables.
		 */
		++hmp->flushcnt;
		++hmp->voldata.alloc_tid;
		trans->sync_tid = hmp->voldata.alloc_tid;
		trans->real_tid = trans->sync_tid;
		++hmp->voldata.alloc_tid;
		TAILQ_INSERT_TAIL(&hmp->transq, trans, entry);
		if (TAILQ_FIRST(&hmp->transq) != trans) {
			trans->blocked = 1;
			while (trans->blocked) {
				lksleep(&trans->sync_tid, &hmp->voldatalk,
					0, "h2multf", hz);
			}
		}
	} else if (hmp->flushcnt == 0) {
		/*
		 * No flushes are pending, we can go.
		 */
		TAILQ_INSERT_TAIL(&hmp->transq, trans, entry);
		trans->sync_tid = hmp->voldata.alloc_tid;
		trans->real_tid = trans->sync_tid;

		/* XXX improve/optimize inode allocation */
	} else {
		/*
		 * One or more flushes are pending.  We insert after
		 * the current flush and may block.  We have priority
		 * over any flushes that are not the current flush.
		 *
		 * TRANS_BUFCACHE transactions cannot block.
		 */
		TAILQ_FOREACH(head, &hmp->transq, entry) {
			if (head->flags & HAMMER2_TRANS_ISFLUSH)
				break;
		}
		KKASSERT(head);
		TAILQ_INSERT_AFTER(&hmp->transq, head, trans, entry);
		trans->sync_tid = head->real_tid + 1;
		trans->real_tid = trans->sync_tid;

		if ((trans->flags & HAMMER2_TRANS_BUFCACHE) == 0) {
			if (TAILQ_FIRST(&hmp->transq) != head) {
				trans->blocked = 1;
				while (trans->blocked) {
					lksleep(&trans->sync_tid,
						&hmp->voldatalk, 0,
						"h2multf", hz);
				}
			}
		}
	}
	if (flags & HAMMER2_TRANS_NEWINODE)
		trans->inode_tid = hmp->voldata.inode_tid++;
	hammer2_voldata_unlock(hmp, 0);
}

void
hammer2_trans_done(hammer2_trans_t *trans)
{
	hammer2_mount_t *hmp;
	hammer2_trans_t *head;
	hammer2_trans_t *scan;

	if (trans->pmp)
		hmp = trans->pmp->cluster.chains[0]->hmp;
	else
		hmp = trans->hmp_single;

	/*
	 * Remove and adjust flushcnt
	 */
	hammer2_voldata_lock(hmp);
	TAILQ_REMOVE(&hmp->transq, trans, entry);
	if (trans->flags & HAMMER2_TRANS_ISFLUSH)
		--hmp->flushcnt;

	/*
	 * Unblock the head of the queue and any additional transactions
	 * up to the next flush.
	 */
	head = TAILQ_FIRST(&hmp->transq);
	if (head && head->blocked) {
		head->blocked = 0;
		wakeup(&head->sync_tid);

		scan = TAILQ_NEXT(head, entry);
		while (scan && (scan->flags & HAMMER2_TRANS_ISFLUSH) == 0) {
			if (scan->blocked) {
				scan->blocked = 0;
				wakeup(&scan->sync_tid);
			}
			scan = TAILQ_NEXT(scan, entry);
		}
	}
	hammer2_voldata_unlock(hmp, 0);
}

/*
 * Flush the chain and all modified sub-chains through the specified
 * synchronization point (sync_tid), propagating parent chain modifications
 * and mirror_tid updates back up as needed.  Since we are recursing downward
 * we do not have to deal with the complexities of multi-homed chains (chains
 * with multiple parents).
 *
 * Caller must have interlocked against any non-flush-related modifying
 * operations in progress whos modify_tid values are less than or equal
 * to the passed sync_tid.
 *
 * Caller must have already vetted synchronization points to ensure they
 * are properly flushed.  Only snapshots and cluster flushes can create
 * these sorts of synchronization points.
 *
 * This routine can be called from several places but the most important
 * is from the hammer2_vop_reclaim() function.  We want to try to completely
 * clean out the inode structure to prevent disconnected inodes from
 * building up and blowing out the kmalloc pool.  However, it is not actually
 * necessary to flush reclaimed inodes to maintain HAMMER2's crash recovery
 * capability.
 *
 * chain is locked on call and will remain locked on return.  If a flush
 * occured, the chain's MOVED bit will be set indicating that its parent
 * (which is not part of the flush) should be updated.  The chain may be
 * replaced by the call.
 */
void
hammer2_chain_flush(hammer2_trans_t *trans, hammer2_chain_t **chainp)
{
	hammer2_chain_t *chain = *chainp;
	hammer2_chain_t *scan;
	hammer2_chain_core_t *core;
	hammer2_flush_info_t info;
	int loops;

	/*
	 * Execute the recursive flush and handle deferrals.
	 *
	 * Chains can be ridiculously long (thousands deep), so to
	 * avoid blowing out the kernel stack the recursive flush has a
	 * depth limit.  Elements at the limit are placed on a list
	 * for re-execution after the stack has been popped.
	 */
	bzero(&info, sizeof(info));
	TAILQ_INIT(&info.flush_list);
	info.trans = trans;
	info.sync_tid = trans->sync_tid;
	info.cache_index = -1;

	core = chain->core;
#if FLUSH_DEBUG
	kprintf("CHAIN FLUSH trans %p.%016jx chain %p.%d mod %016jx upd %016jx\n", trans, trans->sync_tid, chain, chain->bref.type, chain->modify_tid, core->update_lo);
#endif

	/*
	 * Extra ref needed because flush_core expects it when replacing
	 * chain.
	 */
	hammer2_chain_ref(chain);
	loops = 0;

	for (;;) {
		/*
		 * Unwind deep recursions which had been deferred.  This
		 * can leave MOVED set for these chains, which will be
		 * handled when we [re]flush chain after the unwind.
		 */
		while ((scan = TAILQ_FIRST(&info.flush_list)) != NULL) {
			KKASSERT(scan->flags & HAMMER2_CHAIN_DEFERRED);
			TAILQ_REMOVE(&info.flush_list, scan, flush_node);
			atomic_clear_int(&scan->flags, HAMMER2_CHAIN_DEFERRED);

			/*
			 * Now that we've popped back up we can do a secondary
			 * recursion on the deferred elements.
			 *
			 * NOTE: hammer2_chain_flush() may replace scan.
			 */
			if (hammer2_debug & 0x0040)
				kprintf("deferred flush %p\n", scan);
			hammer2_chain_lock(scan, HAMMER2_RESOLVE_MAYBE);
			hammer2_chain_drop(scan);	/* ref from deferral */
			hammer2_chain_flush(trans, &scan);
			hammer2_chain_unlock(scan);
		}

		/*
		 * [re]flush chain.
		 */
		info.diddeferral = 0;
		hammer2_chain_flush_core(&info, &chain);
#if FLUSH_DEBUG
		kprintf("flush_core_done parent=<base> chain=%p.%d %08x\n",
			chain, chain->bref.type, chain->flags);
#endif

		/*
		 * Only loop if deep recursions have been deferred.
		 */
		if (TAILQ_EMPTY(&info.flush_list))
			break;

		if (++loops % 1000 == 0) {
			kprintf("hammer2_chain_flush: excessive loops on %p\n",
				chain);
			if (hammer2_debug & 0x100000)
				Debugger("hell4");
		}
	}
	hammer2_chain_drop(chain);
	*chainp = chain;
}

/*
 * This is the core of the chain flushing code.  The chain is locked by the
 * caller and must also have an extra ref on it by the caller, and remains
 * locked and will have an extra ref on return.
 *
 * If the flush accomplished any work chain will be flagged MOVED
 * indicating a copy-on-write propagation back up is required.
 * Deep sub-nodes may also have been entered onto the deferral list.
 * MOVED is never set on the volume root.
 *
 * NOTE: modify_tid is different from MODIFIED.  modify_tid is updated
 *	 only when a chain is specifically modified, and not updated
 *	 for copy-on-write propagations.  MODIFIED is set on any modification
 *	 including copy-on-write propagations.
 *
 * NOTE: We are responsible for updating chain->bref.mirror_tid and
 *	 core->update_lo  The caller is responsible for processing us into
 *	 our parent (if any).
 *
 *	 We are also responsible for updating chain->core->update_lo to
 *	 prevent repeated recursions due to deferrals.
 */
static void
hammer2_chain_flush_core(hammer2_flush_info_t *info, hammer2_chain_t **chainp)
{
	hammer2_chain_t *chain = *chainp;
	hammer2_mount_t *hmp;
	hammer2_blockref_t *bref;
	hammer2_chain_core_t *core;
	char *bdata;
	hammer2_io_t *dio;
	int error;
	int diddeferral;

	hmp = chain->hmp;
	core = chain->core;
	diddeferral = info->diddeferral;

#if FLUSH_DEBUG
	if (info->parent)
		kprintf("flush_core %p->%p.%d %08x (%s)\n",
			info->parent, chain, chain->bref.type,
			chain->flags,
			((chain->bref.type == HAMMER2_BREF_TYPE_INODE) ?
				(char *)chain->data->ipdata.filename : "?"));
	else
		kprintf("flush_core NULL->%p.%d %08x (%s)\n",
			chain, chain->bref.type,
			chain->flags,
			((chain->bref.type == HAMMER2_BREF_TYPE_INODE) ?
				(char *)chain->data->ipdata.filename : "?"));
	kprintf("PUSH   %p.%d %08x mod=%016jx del=%016jx mirror=%016jx (sync %016jx, update_lo %016jx)\n", chain, chain->bref.type, chain->flags, chain->modify_tid, chain->delete_tid, chain->bref.mirror_tid, info->sync_tid, core->update_lo);
#endif

	/*
	 * Check if we even have any work to do.
	 *
	 * We do not update core->update_lo because there might be other
	 * paths to the core and we haven't actually checked it.
	 *
	 * This bit of code is capable of short-cutting entire sub-trees
	 * if they have not been touched.
	 */
	if ((chain->flags & HAMMER2_CHAIN_MODIFIED) == 0 &&
	    (core->update_lo >= info->sync_tid ||
	     chain->bref.mirror_tid >= info->sync_tid ||
	     chain->bref.mirror_tid >= core->update_hi)) {
		KKASSERT(chain->modify_tid <= info->sync_tid);
		/* don't update update_lo, there may be other paths to core */
		/* don't update bref.mirror_tid, scan2 is not called */
		return;
	}

	/*
	 * Ignore chains modified beyond the current flush point.  These
	 * will be treated as if they did not exist.  Subchains with lower
	 * modify_tid's will still be accessible via other parents.
	 *
	 * Do not update bref.mirror_tid here, it will interfere with
	 * synchronization.  e.g. inode flush tid 1, concurrent D-D tid 2,
	 * then later on inode flush tid 2.  If we were to set mirror_tid
	 * to 1 during inode flush tid 1 the blockrefs would only be partially
	 * updated (and likely panic).
	 *
	 * Do not update core->update_lo here, there might be other paths
	 * to the core and we haven't actually flushed it.
	 *
	 * (vchain and fchain are exceptions since they cannot be duplicated)
	 */
	if (chain->modify_tid > info->sync_tid &&
	    chain != &hmp->fchain && chain != &hmp->vchain) {
		chain->debug_reason = (chain->debug_reason & ~255) | 5;
		/* do not update bref.mirror_tid, scan2 ignores chain */
		/* do not update core->update_lo, there may be another path */
		return;
	}

retry:
	/*
	 * Early handling of deleted chains is required to avoid double
	 * recursions.  If the deleted chain has been duplicated then the
	 * flush will have visibility into chain->core via some other chain
	 * and we can safely terminate the operation right here.
	 *
	 * If the deleted chain has not been duplicated then the deletion
	 * is terminal and we must recurse to deal with any dirty chains
	 * under the deletion, including possibly flushing them out (e.g.
	 * open descriptor on an unlinked file).
	 */
	if (chain->delete_tid <= info->sync_tid &&
	    (chain->flags & HAMMER2_CHAIN_DUPLICATED)) {
		chain->debug_reason = (chain->debug_reason & ~255) | 9;
		if (chain->flags & HAMMER2_CHAIN_MODIFIED) {
#if 0
			/*
			 * XXX should be able to invalidate the buffer here.
			 * XXX problem if reused, snapshotted, or reactivated.
			 */
			if (chain->dio) {
				hammer2_io_setinval(chain->dio, chain->bytes);
			}
#endif
			if ((chain->flags & HAMMER2_CHAIN_MOVED) == 0) {
				hammer2_chain_ref(chain);
				atomic_set_int(&chain->flags,
					       HAMMER2_CHAIN_MOVED);
			}
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_MODIFIED);
			hammer2_chain_drop(chain);
		}

		/*
		 * Update mirror_tid, indicating that chain is synchronized
		 * on its modification and block table.  This probably isn't
		 * needed since scan2 should ignore deleted chains anyway.
		 */
		if (chain->bref.mirror_tid < info->sync_tid)
			chain->bref.mirror_tid = info->sync_tid;
		/* do not update core->update_lo, there may be another path */
		return;
	}

	/*
	 * Recurse if we are not up-to-date.  Once we are done we will
	 * update update_lo if there were no deferrals.  update_lo can become
	 * higher than update_hi and is used to prevent re-recursions during
	 * the same flush cycle.
	 *
	 * update_hi was already checked and prevents initial recursions on
	 * subtrees which have not been modified.
	 *
	 * NOTE: We must recurse whether chain is flagged DELETED or not.
	 *	 However, if it is flagged DELETED we limit sync_tid to
	 *	 delete_tid to ensure that the chain's bref.mirror_tid is
	 *	 not fully updated and causes it to miss the non-DELETED
	 *	 path.
	 *
	 * NOTE: If a deferral occurs hammer2_chain_flush() will flush the
	 *	 deferred chain independently which will update it's
	 *	 bref.mirror_tid and prevent it from deferred again.
	 */
	if (chain->bref.mirror_tid < info->sync_tid &&
	    chain->bref.mirror_tid < core->update_hi) {
		hammer2_chain_t *saved_parent;
		hammer2_chain_layer_t *layer;
		int saved_domodify;
		int save_gen;

		/*
		 * Races will bump update_hi above trans->sync_tid causing
		 * us to catch the issue in a later flush.
		 *
		 * We don't want to set our chain to MODIFIED gratuitously.
		 *
		 * We need an extra ref on chain because we are going to
		 * release its lock temporarily in our child loop.
		 */

		/*
		 * Run two passes.  The first pass handles MODIFIED and
		 * update_lo recursions while the second pass handles
		 * MOVED chains on the way back up.
		 *
		 * If the stack gets too deep we defer the chain.   Since
		 * hammer2_chain_core's can be shared at multiple levels
		 * in the tree, we may encounter a chain that we had already
		 * deferred.  We could undefer it but it will probably just
		 * defer again so it is best to leave it deferred.
		 *
		 * Scan1 is recursive.
		 *
		 * NOTE: The act of handling a modified/submodified chain can
		 *	 cause the MOVED Flag to be set.  It can also be set
		 *	 via hammer2_chain_delete() and in other situations.
		 *
		 * NOTE: RB_SCAN() must be used instead of RB_FOREACH()
		 *	 because children can be physically removed during
		 *	 the scan.
		 *
		 * NOTE: We would normally not care about insertions except
		 *	 that some insertions might occur from the flush
		 *	 itself, so loop on generation number changes.
		 */
		saved_parent = info->parent;
		saved_domodify = info->domodify;
		info->parent = chain;
		info->domodify = 0;
		chain->debug_reason = (chain->debug_reason & ~255) | 6;

		if (chain->flags & HAMMER2_CHAIN_DEFERRED) {
			++info->diddeferral;
		} else if (info->depth == HAMMER2_FLUSH_DEPTH_LIMIT) {
			if ((chain->flags & HAMMER2_CHAIN_DEFERRED) == 0) {
				hammer2_chain_ref(chain);
				TAILQ_INSERT_TAIL(&info->flush_list,
						  chain, flush_node);
				atomic_set_int(&chain->flags,
					       HAMMER2_CHAIN_DEFERRED);
			}
			++info->diddeferral;
		} else {
			spin_lock(&core->cst.spin);
			KKASSERT(core->good == 0x1234 && core->sharecnt > 0);
			do {
				save_gen = core->generation;
				TAILQ_FOREACH_REVERSE(layer, &core->layerq,
						      h2_layer_list, entry) {
					++layer->refs;
					KKASSERT(layer->good == 0xABCD);
					RB_SCAN(hammer2_chain_tree,
						&layer->rbtree,
						NULL, hammer2_chain_flush_scan1,
						info);
					--layer->refs;
				}
			} while (core->generation != save_gen);
			spin_unlock(&core->cst.spin);
		}

		if (info->parent != chain) {
			kprintf("ZZZ\n");
			hammer2_chain_drop(chain);
			hammer2_chain_ref(info->parent);
		}
		chain = info->parent;

		/*
		 * We unlock the parent during the scan1 recursion, parent
		 * may have been deleted out from under us.
		 *
		 * parent may have been destroyed out from under us
		 *
		 * parent may have been synchronously flushed due to aliasing
		 * via core (is this possible?).
		 */
		if (chain->delete_tid <= info->sync_tid &&
		    (chain->flags & HAMMER2_CHAIN_DUPLICATED)) {
			kprintf("xxx\n");
			goto retry;
		}
		if (chain->bref.mirror_tid >= info->sync_tid ||
		    chain->bref.mirror_tid >= core->update_hi) {
			kprintf("yyy\n");
			goto retry;
		}

		/*
		 * If any deferral occurred we must set domodify to 0 to avoid
		 * potentially modifying the parent twice (now and when we run
		 * the deferral list), as doing so could cause the blockref
		 * update to run on a block array which has already been
		 * updated.
		 */
		if (info->domodify && diddeferral != info->diddeferral)
			info->domodify = 0;

		/*
		 * We are responsible for setting the parent into a modified
		 * state before we scan the children to update the parent's
		 * block table.  This must essentially be done as an atomic
		 * operation (the parent must remain locked throughout the
		 * operation), otherwise other transactions can squeeze a
		 * delete-duplicate in and create block table havoc.
		 *
		 * Care must be taken to not try to update the parent twice
		 * during the current flush cycle, which would cause more
		 * havoc.  It's so important that we assert that we haven't
		 * double-flushed a parent below by testing modify_tid.
		 *
		 * NOTE: Blockrefs are only updated on live chains.
		 *
		 * NOTE: Modifying the parent generally causes a
		 *	 delete-duplicate to occur from within the flush
		 *	 itself, with an allocation from the freemap occuring
		 *	 as an additional side-effect.
		 *
		 * NOTE: If the parent was deleted our modified chain will
		 *	 also be marked deleted, but since it inherits the
		 *	 parent's delete_tid it will still appear to be
		 *	 'live' for the purposes of the flush.
		 */
		if (info->domodify && !h2ignore_deleted(info, chain)) {
			KKASSERT(chain->modify_tid < info->sync_tid);

			/*
			 * The scan1 loop and/or flush_core is reentrant,
			 * particularly when core->generation changes.  To
			 * avoid havoc we have to prevent repetitive
			 * delete-duplicates of the same chain.
			 *
			 * After executing the modify set the original chain's
			 * bref.mirror_tid to prevent any reentrancy during
			 * the current flush cycle.
			 */
			hammer2_chain_modify(info->trans, &info->parent,
					     HAMMER2_MODIFY_NO_MODIFY_TID);
			if (info->parent != chain) {
				if (chain->bref.mirror_tid < info->sync_tid)
					chain->bref.mirror_tid = info->sync_tid;
				hammer2_chain_drop(chain);
				hammer2_chain_ref(info->parent);
			}
			chain = info->parent;
		}
		chain->debug_reason = (chain->debug_reason & ~255) | 7;

		KKASSERT(chain == info->parent);

		/*
		 * Handle successfully flushed children who are in the MOVED
		 * state on the way back up the recursion.  This can have
		 * the side-effect of clearing MOVED.
		 *
		 * Scan2 may replace info->parent.  If it does it will also
		 * replace the extra ref we made.
		 *
		 * Scan2 is non-recursive.
		 */
		if (diddeferral != info->diddeferral) {
			spin_lock(&core->cst.spin);
		} else {
			KKASSERT(chain == info->parent);
			KKASSERT(info->domodify == 0 ||
				 (chain->flags & HAMMER2_CHAIN_FLUSHED) == 0);
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_FLUSHED);
			spin_lock(&core->cst.spin);
			KKASSERT(core->good == 0x1234 && core->sharecnt > 0);
			KKASSERT(info->parent->core == core);
			TAILQ_FOREACH_REVERSE(layer, &core->layerq,
					      h2_layer_list, entry) {
				info->pass = 1;
				++layer->refs;
				KKASSERT(layer->good == 0xABCD);
				RB_SCAN(hammer2_chain_tree, &layer->rbtree,
					NULL, hammer2_chain_flush_scan2, info);
				info->pass = 2;
				RB_SCAN(hammer2_chain_tree, &layer->rbtree,
					NULL, hammer2_chain_flush_scan2, info);
				--layer->refs;
			}

			/*
			 * chain is now considered up-to-date, adjust
			 * bref.mirror_tid and update_lo before running
			 * pass3.
			 *
			 * (no deferral in this path)
			 */
			if (core->update_lo < info->sync_tid)
				core->update_lo = info->sync_tid;

			TAILQ_FOREACH_REVERSE(layer, &core->layerq,
					      h2_layer_list, entry) {
				info->pass = 3;
				++layer->refs;
				KKASSERT(layer->good == 0xABCD);
				RB_SCAN(hammer2_chain_tree, &layer->rbtree,
					NULL, hammer2_chain_flush_scan2, info);
				--layer->refs;
				KKASSERT(info->parent->core == core);
			}
		}

		/*
		 * info->parent must not have been replaced again
		 */
		KKASSERT(info->parent == chain);

		chain->debug_reason = (chain->debug_reason & ~255) | 8;
		*chainp = chain;

		hammer2_chain_layer_check_locked(chain->hmp, core);
		spin_unlock(&core->cst.spin);

		info->parent = saved_parent;
		info->domodify = saved_domodify;
		KKASSERT(chain->refs > 1);
	} else {
		/*
		 * There is no deferral in this path.  Chain is now
		 * considered up-to-date.
		 *
		 * Adjust update_lo now and bref.mirror_tid will be
		 * updated a bit later on the fall-through.
		 */
		if (core->update_lo < info->sync_tid)
			core->update_lo = info->sync_tid;
	}

#if FLUSH_DEBUG
	kprintf("POP    %p.%d defer=%d\n", chain, chain->bref.type, diddeferral);
#endif

	/*
	 * Do not flush chain if there were any deferrals.  It will be
	 * retried later after the deferrals are independently handled.
	 * Do not update update_lo or bref.mirror_tid.
	 */
	if (diddeferral != info->diddeferral) {
		chain->debug_reason = (chain->debug_reason & ~255) | 99;
		if (hammer2_debug & 0x0008) {
			kprintf("%*.*s} %p/%d %04x (deferred)",
				info->depth, info->depth, "",
				chain, chain->refs, chain->flags);
		}
		/* do not update core->update_lo */
		/* do not update bref.mirror_tid */
		return;
	}

	/*
	 * Non-deferral path, chain is now deterministically being flushed.
	 * We've finished running the recursion and the blockref update.
	 *
	 * update bref.mirror_tid.  update_lo has already been updated.
	 */
	if (chain->bref.mirror_tid < info->sync_tid)
		chain->bref.mirror_tid = info->sync_tid;

	/*
	 * Deal with deleted and destroyed chains on the way back up.
	 *
	 * Deleted inodes may still be active due to open descriptors so
	 * test whether the inode has been DESTROYED (aka deactivated after
	 * being unlinked) or not.
	 *
	 * Otherwise a delted chain can be optimized by clearing MODIFIED
	 * without bothering to write it out.
	 *
	 * NOTE: We optimize this by noting that only 'inode' chains require
	 *	 this treatment.  When a file with an open descriptor is
	 *	 deleted only its inode is marked deleted.  Other deletions,
	 *	 such as indirect block deletions, will no longer be visible
	 *	 to the live filesystem and do not need to be updated.
	 */
	if (h2ignore_deleted(info, chain)) {
		/*
		 * At the moment we unconditionally set the MOVED bit because
		 * there are situations where it might not have been set due
		 * to similar delete-destroyed optimizations, and the parent
		 * of the parent still may need to be notified of the deletion.
		 */
		if ((chain->flags & HAMMER2_CHAIN_MOVED) == 0) {
			hammer2_chain_ref(chain);
			atomic_set_int(&chain->flags,
				       HAMMER2_CHAIN_MOVED);
		}
		chain->debug_reason = (chain->debug_reason & ~255) | 9;
		if (chain->flags & HAMMER2_CHAIN_MODIFIED) {
#if 0
			/*
			 * XXX should be able to invalidate the buffer here.
			 * XXX problem if reused, snapshotted, or reactivated.
			 */
			if (chain->dio) {
				hammer2_io_setinval(chain->dio, chain->bytes);
			}
#endif
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_MODIFIED);
			hammer2_chain_drop(chain);
		}
		return;
	}

	/*
	 * A degenerate flush might not have flushed anything and thus not
	 * processed modified blocks on the way back up.  Detect the case.
	 *
	 * This case can occur when modifications cross flush boundaries
	 * and cause the submodified recursion to run up multiple parents (?).
	 */
	if ((chain->flags & HAMMER2_CHAIN_MODIFIED) == 0) {
		kprintf("chain %p.%d %08x recursed but wasn't "
			"modified mirr=%016jx "
			"update_lo=%016jx synctid=%016jx\n",
			chain, chain->bref.type, chain->flags,
			chain->bref.mirror_tid,
			core->update_lo, info->sync_tid);
#if 0
		if ((chain->flags & HAMMER2_CHAIN_MOVED) == 0) {
			hammer2_chain_ref(chain);
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_MOVED);
		}
#endif
		chain->debug_reason = (chain->debug_reason & ~255) | 10;
		return;
	}

	chain->debug_reason = (chain->debug_reason & ~255) | 11;

	/*
	 * Issue flush.
	 *
	 * A DESTROYED node that reaches this point must be flushed for
	 * synchronization point consistency.
	 *
	 * Update bref.mirror_tid, clear MODIFIED, and set MOVED.
	 *
	 * The caller will update the parent's reference to this chain
	 * by testing MOVED as long as the modification was in-bounds.
	 *
	 * MOVED is never set on the volume root as there is no parent
	 * to adjust.
	 */
	if (hammer2_debug & 0x1000) {
		kprintf("Flush %p.%d %016jx/%d sync_tid=%016jx data=%016jx\n",
			chain, chain->bref.type,
			chain->bref.key, chain->bref.keybits,
			info->sync_tid, chain->bref.data_off);
	}
	if (hammer2_debug & 0x2000) {
		Debugger("Flush hell");
	}

	atomic_clear_int(&chain->flags, HAMMER2_CHAIN_MODIFIED);

	if ((chain->flags & HAMMER2_CHAIN_MOVED) ||
	    chain == &hmp->vchain ||
	    chain == &hmp->fchain) {
		/*
		 * Drop the ref from the MODIFIED bit we cleared,
		 * net -1 ref.
		 */
		hammer2_chain_drop(chain);
	} else {
		/*
		 * Drop the ref from the MODIFIED bit we cleared and
		 * set a ref for the MOVED bit we are setting.  Net 0 refs.
		 */
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_MOVED);
	}

	/*
	 * If this is part of a recursive flush we can go ahead and write
	 * out the buffer cache buffer and pass a new bref back up the chain
	 * via the MOVED bit.
	 *
	 * Volume headers are NOT flushed here as they require special
	 * processing.
	 */
	switch(chain->bref.type) {
	case HAMMER2_BREF_TYPE_FREEMAP:
		hammer2_modify_volume(hmp);
		hmp->voldata.freemap_tid = hmp->fchain.bref.mirror_tid;
		break;
	case HAMMER2_BREF_TYPE_VOLUME:
		/*
		 * The free block table is flushed by hammer2_vfs_sync()
		 * before it flushes vchain.  We must still hold fchain
		 * locked while copying voldata to volsync, however.
		 */
		hammer2_chain_lock(&hmp->fchain, HAMMER2_RESOLVE_ALWAYS);
#if 0
		if ((hmp->fchain.flags & HAMMER2_CHAIN_MODIFIED) ||
		    hmp->voldata.freemap_tid < info->trans->sync_tid) {
			/* this will modify vchain as a side effect */
			hammer2_chain_t *tmp = &hmp->fchain;
			hammer2_chain_flush(info->trans, &tmp);
			KKASSERT(tmp == &hmp->fchain);
		}
#endif

		/*
		 * There is no parent to our root vchain and fchain to
		 * synchronize the bref to, their updated mirror_tid's
		 * must be synchronized to the volume header.
		 */
		hmp->voldata.mirror_tid = chain->bref.mirror_tid;
		/*hmp->voldata.freemap_tid = hmp->fchain.bref.mirror_tid;*/

		/*
		 * The volume header is flushed manually by the syncer, not
		 * here.  All we do here is adjust the crc's.
		 */
		KKASSERT(chain->data != NULL);
		KKASSERT(chain->dio == NULL);

		hmp->voldata.icrc_sects[HAMMER2_VOL_ICRC_SECT1]=
			hammer2_icrc32(
				(char *)&hmp->voldata +
				 HAMMER2_VOLUME_ICRC1_OFF,
				HAMMER2_VOLUME_ICRC1_SIZE);
		hmp->voldata.icrc_sects[HAMMER2_VOL_ICRC_SECT0]=
			hammer2_icrc32(
				(char *)&hmp->voldata +
				 HAMMER2_VOLUME_ICRC0_OFF,
				HAMMER2_VOLUME_ICRC0_SIZE);
		hmp->voldata.icrc_volheader =
			hammer2_icrc32(
				(char *)&hmp->voldata +
				 HAMMER2_VOLUME_ICRCVH_OFF,
				HAMMER2_VOLUME_ICRCVH_SIZE);
		hmp->volsync = hmp->voldata;
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_VOLUMESYNC);
		hammer2_chain_unlock(&hmp->fchain);
		break;
	case HAMMER2_BREF_TYPE_DATA:
		/*
		 * Data elements have already been flushed via the logical
		 * file buffer cache.  Their hash was set in the bref by
		 * the vop_write code.
		 *
		 * Make sure any device buffer(s) have been flushed out here.
		 * (there aren't usually any to flush).
		 */
#if 0
		/* XXX */
		/* chain and chain->bref, NOWAIT operation */
#endif
		break;
#if 0
	case HAMMER2_BREF_TYPE_INDIRECT:
		/*
		 * Indirect blocks may be in an INITIAL state.  Use the
		 * chain_lock() call to ensure that the buffer has been
		 * instantiated (even though it is already locked the buffer
		 * might not have been instantiated).
		 *
		 * Only write the buffer out if it is dirty, it is possible
		 * the operating system had already written out the buffer.
		 */
		hammer2_chain_lock(chain, HAMMER2_RESOLVE_ALWAYS);
		KKASSERT(chain->dio != NULL);

		chain->data = NULL;
		hammer2_io_bqrelse(&chain->dio);
		hammer2_chain_unlock(chain);
		break;
#endif
	case HAMMER2_BREF_TYPE_INDIRECT:
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		/*
		 * Device-backed.  Buffer will be flushed by the sync
		 * code XXX.
		 */
		KKASSERT((chain->flags & HAMMER2_CHAIN_EMBEDDED) == 0);
		break;
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
	default:
		/*
		 * Embedded elements have to be flushed out.
		 * (Basically just BREF_TYPE_INODE).
		 */
		KKASSERT(chain->flags & HAMMER2_CHAIN_EMBEDDED);
		KKASSERT(chain->data != NULL);
		KKASSERT(chain->dio == NULL);
		bref = &chain->bref;

		KKASSERT((bref->data_off & HAMMER2_OFF_MASK) != 0);
		KKASSERT(HAMMER2_DEC_CHECK(chain->bref.methods) ==
			 HAMMER2_CHECK_ISCSI32 ||
			 HAMMER2_DEC_CHECK(chain->bref.methods) ==
			 HAMMER2_CHECK_FREEMAP);

		/*
		 * The data is embedded, we have to acquire the
		 * buffer cache buffer and copy the data into it.
		 */
		error = hammer2_io_bread(hmp, bref->data_off, chain->bytes,
					 &dio);
		KKASSERT(error == 0);
		bdata = hammer2_io_data(dio, bref->data_off);

		/*
		 * Copy the data to the buffer, mark the buffer
		 * dirty, and convert the chain to unmodified.
		 */
		bcopy(chain->data, bdata, chain->bytes);
		hammer2_io_bdwrite(&dio);

		switch(HAMMER2_DEC_CHECK(chain->bref.methods)) {
		case HAMMER2_CHECK_FREEMAP:
			chain->bref.check.freemap.icrc32 =
				hammer2_icrc32(chain->data, chain->bytes);
			break;
		case HAMMER2_CHECK_ISCSI32:
			chain->bref.check.iscsi32.value =
				hammer2_icrc32(chain->data, chain->bytes);
			break;
		default:
			panic("hammer2_flush_core: bad crc type");
			break; /* NOT REACHED */
		}
		if (chain->bref.type == HAMMER2_BREF_TYPE_INODE)
			++hammer2_iod_meta_write;
		else
			++hammer2_iod_indr_write;
	}
}

/*
 * Flush helper scan1 (recursive)
 *
 * Flushes the children of the caller's chain (parent) and updates
 * the blockref, restricted by sync_tid.
 *
 * Ripouts during the loop should not cause any problems.  Because we are
 * flushing to a synchronization point, modification races will occur after
 * sync_tid and do not have to be flushed anyway.
 *
 * It is also ok if the parent is chain_duplicate()'d while unlocked because
 * the delete/duplication will install a delete_tid that is still larger than
 * our current sync_tid.
 *
 * WARNING! If we do not call chain_flush_core we must update bref.mirror_tid
 *	    ourselves.
 */
static int
hammer2_chain_flush_scan1(hammer2_chain_t *child, void *data)
{
	hammer2_flush_info_t *info = data;
	hammer2_trans_t *trans = info->trans;
	hammer2_chain_t *parent = info->parent;
	int	diddeferral;

	if (hammer2_debug & 0x80000)
		Debugger("hell3");
	diddeferral = info->diddeferral;

	/*
	 * Child is beyond the flush synchronization zone, don't persue.
	 * Remember that modifications generally delete-duplicate so if the
	 * sub-tree is dirty another child will get us there.  But not this
	 * one.
	 *
	 * Or MODIFIED is not set and child is already fully synchronized
	 * with its sub-tree.  Don't persue.
	 *
	 * (child can never be fchain or vchain so a special check isn't
	 *  needed).
	 */
	if (child->modify_tid > trans->sync_tid) {
		KKASSERT(child->delete_tid >= child->modify_tid);
		child->debug_reason = (child->debug_reason & ~255) | 1;
		/* do not update child->core->update_lo, core not flushed */
		/* do not update core->update_lo, there may be another path */
		/* do not update mirror_tid, scan2 will ignore chain */
		return (0);
	}

	/*
	 * We must ref the child before unlocking the spinlock.
	 *
	 * The caller has added a ref to the parent so we can temporarily
	 * unlock it in order to lock the child.
	 */
	hammer2_chain_ref(child);
	spin_unlock(&parent->core->cst.spin);

	hammer2_chain_unlock(parent);
	hammer2_chain_lock(child, HAMMER2_RESOLVE_MAYBE);

#if 0
	/*
	 * This isn't working atm, it seems to be causing necessary
	 * updates to be thrown away, probably due to aliasing, resulting
	 * base_insert/base_delete panics.
	 */
	/*
	 * The DESTROYED flag can only be initially set on an unreferenced
	 * deleted inode and will propagate downward via the mechanic below.
	 * Such inode chains have been deleted for good and should no longer
	 * be subject to delete/duplication.
	 *
	 * This optimization allows the inode reclaim (destroy unlinked file
	 * on vnode reclamation after last close) to be flagged by just
	 * setting HAMMER2_CHAIN_DESTROYED at the top level and then will
	 * cause the chains to be terminated and related buffers to be
	 * invalidated and not flushed out.
	 *
	 * We have to be careful not to propagate the DESTROYED flag if
	 * the destruction occurred after our flush sync_tid.
	 */
	if (parent->delete_tid <= trans->sync_tid &&
	    (parent->flags & HAMMER2_CHAIN_DESTROYED) &&
	    (child->flags & HAMMER2_CHAIN_DESTROYED) == 0) {
		/*
		 * Force downward recursion by bringing update_hi up to
		 * at least sync_tid, and setting the DESTROYED flag.
		 * Parent's mirror_tid has not yet been updated.
		 *
		 * We do not mark the child DELETED because this would
		 * cause unnecessary modifications/updates.  Instead, the
		 * DESTROYED flag propagates downward and causes the flush
		 * to ignore any pre-existing modified chains.
		 *
		 * Vnode reclamation may have forced update_hi to MAX_TID
		 * (we do this because there was no transaction at the time).
		 * In this situation bring it down to something reasonable
		 * so the elements being destroyed can be retired.
		 */
		atomic_set_int(&child->flags, HAMMER2_CHAIN_DESTROYED);
		spin_lock(&child->core->cst.spin);
		if (child->core->update_hi < trans->sync_tid)
			child->core->update_hi = trans->sync_tid;
		spin_unlock(&child->core->cst.spin);
	}
#endif

	/*
	 * No recursion needed if neither the child or anything under it
	 * was messed with.
	 */
	if ((child->flags & HAMMER2_CHAIN_MODIFIED) == 0 &&
	    child->core->update_lo >= info->sync_tid) {
		child->debug_reason = (child->debug_reason & ~255) | 2;
		if (child->bref.mirror_tid < info->sync_tid)
			child->bref.mirror_tid = info->sync_tid;
		goto skip;
	}

	/*
	 * Re-check original pre-lock conditions after locking.
	 */
	if (child->modify_tid > trans->sync_tid) {
		child->debug_reason = (child->debug_reason & ~255) | 3;
		hammer2_chain_unlock(child);
		hammer2_chain_drop(child);
		hammer2_chain_lock(parent, HAMMER2_RESOLVE_MAYBE);
		spin_lock(&parent->core->cst.spin);
		return (0);
	}

	if ((child->flags & HAMMER2_CHAIN_MODIFIED) == 0 &&
	    child->core->update_lo >= info->sync_tid) {
		child->debug_reason = (child->debug_reason & ~255) | 4;
		if (child->bref.mirror_tid < info->sync_tid)
			child->bref.mirror_tid = info->sync_tid;
		goto skip;
	}

	/*
	 * Recurse and collect deferral data.
	 */
	++info->depth;
	hammer2_chain_flush_core(info, &child);
	--info->depth;

skip:
	/*
	 * Check the conditions that could cause SCAN2 to modify the parent.
	 * Modify the parent here instead of in SCAN2, which would cause
	 * rollup chicken-and-egg races.
	 *
	 * Scan2 is expected to update bref.mirror_tid in the domodify case,
	 * but will skip the child otherwise giving us the responsibility to
	 * update bref.mirror_tid.
	 *
	 * WARNING!  Do NOT update the child's bref.mirror_tid right here,
	 *	     even if there was no deferral.  Doing so would cause
	 *	     confusion with the child's block array state in a
	 *	     future flush.
	 */
	if (h2ignore_deleted(info, parent)) {
		/*
		 * Special optimization matching similar tests done in
		 * flush_core, scan1, and scan2.  Avoid updating the block
		 * table in the parent if the parent is no longer visible.
		 * A deleted parent is no longer visible unless it's an
		 * inode (in which case it might have an open fd).. the
		 * DESTROYED flag must also be checked for inodes.
		 */
		;
	} else if (child->delete_tid <= trans->sync_tid &&
		   child->delete_tid > parent->bref.mirror_tid &&
		   child->modify_tid <= parent->bref.mirror_tid) {
		info->domodify = 1;
	} else if (child->delete_tid > trans->sync_tid &&
		   child->modify_tid > parent->bref.mirror_tid) {
		info->domodify = 1;		/* base insertion */
	}

	/*
	 * Relock to continue the loop
	 */
	hammer2_chain_unlock(child);
	hammer2_chain_lock(parent, HAMMER2_RESOLVE_MAYBE);
	hammer2_chain_drop(child);
	KKASSERT(info->parent == parent);

	spin_lock(&parent->core->cst.spin);
	return (0);
}

/*
 * Flush helper scan2 (non-recursive)
 *
 * This pass on a chain's children propagates any MOVED or DELETED
 * elements back up the chain towards the root after those elements have
 * been fully flushed.  Unlike scan1, this function is NOT recursive and
 * the parent remains locked across the entire scan.
 *
 * SCAN2 is called twice, once with pass set to 1 and once with it set to 2.
 * We have to do this so base[] elements can be deleted in pass 1 to make
 * room for adding new elements in pass 2.
 *
 * This function also rolls up storage statistics.
 *
 * NOTE!  A deletion is a visbility issue, there can still be references to
 *	  deleted elements (for example, to an unlinked file which is still
 *	  open), and there can also be multiple chains pointing to the same
 *	  bref where some are deleted and some are not (for example due to
 *	  a rename).   So a chain marked for deletion is basically considered
 *	  to be live until it is explicitly destroyed or until its ref-count
 *	  reaches zero (also implying that MOVED and MODIFIED are clear).
 *
 * NOTE!  Info->parent will be locked but will only be instantiated/modified
 *	  if it is either MODIFIED or if scan1 determined that block table
 *	  updates will occur.
 *
 * NOTE!  SCAN2 is responsible for updating child->bref.mirror_tid only in
 *	  the case where it modifies the parent (does a base insertion
 *	  or deletion).  SCAN1 handled all other cases.
 */
static int
hammer2_chain_flush_scan2(hammer2_chain_t *child, void *data)
{
	hammer2_flush_info_t *info = data;
	hammer2_chain_t *parent = info->parent;
	hammer2_chain_core_t *above = child->above;
	hammer2_mount_t *hmp = child->hmp;
	hammer2_trans_t *trans = info->trans;
	hammer2_blockref_t *base;
	int count;
	int ok;

#if FLUSH_DEBUG
	kprintf("SCAN2 %p.%d %08x mod=%016jx del=%016jx trans=%016jx\n", child, child->bref.type, child->flags, child->modify_tid, child->delete_tid, info->trans->sync_tid);
#endif
	/*
	 * Ignore children created after our flush point, treating them as
	 * if they did not exist).  These children will not cause the parent
	 * to be updated.
	 *
	 * Children deleted after our flush point are treated as having been
	 * created for the purposes of the flush.  The parent's update_hi
	 * will already be higher than our trans->sync_tid so the path for
	 * the next flush is left intact.
	 *
	 * When we encounter such children and the parent chain has not been
	 * deleted, delete/duplicated, or delete/duplicated-for-move, then
	 * the parent may be used to funnel through several flush points.
	 * These chains will still be visible to later flushes due to having
	 * a higher update_hi than we can set in the current flush.
	 */
	if (child->modify_tid > trans->sync_tid) {
		KKASSERT(child->delete_tid >= child->modify_tid);
		goto finalize;
	}

#if 0
	/*
	 * Ignore children which have not changed.  The parent's block table
	 * is already correct.
	 *
	 * XXX The MOVED bit is only cleared when all multi-homed parents
	 *     have flushed, creating a situation where a re-flush can occur
	 *     via a parent which has already flushed.  The hammer2_base_*()
	 *     functions currently have a hack to deal with this case but
	 *     we need something better.
	 */
	if ((child->flags & HAMMER2_CHAIN_MOVED) == 0) {
		KKASSERT((child->flags & HAMMER2_CHAIN_MODIFIED) == 0);
		goto finalize;
	}
#endif

	/*
	 * Make sure child is referenced before we unlock.
	 */
	hammer2_chain_ref(child);
	spin_unlock(&above->cst.spin);

	/*
	 * Parent reflushed after the child has passed them by should skip
	 * due to the modify_tid test. XXX
	 */
	hammer2_chain_lock(child, HAMMER2_RESOLVE_NEVER);
	KKASSERT(child->above == above);
	KKASSERT(parent->core == above);

	/*
	 * The parent's blockref to the child must be deleted or updated.
	 *
	 * This point is not reached on successful DESTROYED optimizations
	 * but can be reached on recursive deletions and restricted flushes.
	 *
	 * The chain_modify here may delete-duplicate the block.  This can
	 * cause a multitude of issues if the block was already modified
	 * by a later (post-flush) transaction.  Primarily blockrefs in
	 * the later block can be out-of-date, so if the situation occurs
	 * we can't throw away the MOVED bit on the current blocks until
	 * the later blocks are flushed (so as to be able to regenerate all
	 * the changes that were made).
	 *
	 * Because flushes are ordered we do not have to make a
	 * modify/duplicate of indirect blocks.  That is, the flush
	 * code does not have to kmalloc or duplicate anything.  We
	 * can adjust the indirect block table in-place and reuse the
	 * chain.  It IS possible that the chain has already been duplicated
	 * or may wind up being duplicated on-the-fly by modifying code
	 * on the frontend.  We simply use the original and ignore such
	 * chains.  However, it does mean we can't clear the MOVED bit.
	 *
	 * XXX recursive deletions not optimized.
	 */

	switch(parent->bref.type) {
	case HAMMER2_BREF_TYPE_INODE:
		/*
		 * Access the inode's block array.  However, there is no
		 * block array if the inode is flagged DIRECTDATA.  The
		 * DIRECTDATA case typicaly only occurs when a hardlink has
		 * been shifted up the tree and the original inode gets
		 * replaced with an OBJTYPE_HARDLINK placeholding inode.
		 */
		if (parent->data &&
		    (parent->data->ipdata.op_flags &
		     HAMMER2_OPFLAG_DIRECTDATA) == 0) {
			base = &parent->data->ipdata.u.blockset.blockref[0];
		} else {
			base = NULL;
		}
		count = HAMMER2_SET_COUNT;
		break;
	case HAMMER2_BREF_TYPE_INDIRECT:
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		if (parent->data)
			base = &parent->data->npdata[0];
		else
			base = NULL;
		count = parent->bytes / sizeof(hammer2_blockref_t);
		break;
	case HAMMER2_BREF_TYPE_VOLUME:
		base = &hmp->voldata.sroot_blockset.blockref[0];
		count = HAMMER2_SET_COUNT;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP:
		base = &parent->data->npdata[0];
		count = HAMMER2_SET_COUNT;
		break;
	default:
		base = NULL;
		count = 0;
		panic("hammer2_chain_flush_scan2: "
		      "unrecognized blockref type: %d",
		      parent->bref.type);
	}

	/*
	 * Don't bother updating a deleted + destroyed parent's blockrefs.
	 * We MUST update deleted + non-destroyed parent's blockrefs since
	 * they could represent an open file.
	 *
	 * Otherwise, we need to be COUNTEDBREFS synchronized for the
	 * hammer2_base_*() functions.
	 *
	 * NOTE: We optimize this by noting that only 'inode' chains require
	 *	 this treatment.  When a file with an open descriptor is
	 *	 deleted only its inode is marked deleted.  Other deletions,
	 *	 such as indirect block deletions, will no longer be visible
	 *	 to the live filesystem and do not need to be updated.
	 *
	 *	 rm -rf's generally wind up setting DESTROYED on the way down
	 *	 and the result is typically that no disk I/O is needed at all
	 *	 when rm -rf'ing an entire directory topology.
	 *
	 *	 This test must match the similar one in flush_core.
	 */
#if FLUSH_DEBUG
	kprintf("SCAN2 base=%p pass=%d PARENT %p.%d DTID=%016jx SYNC=%016jx\n",
		base,
		info->pass, parent, parent->bref.type, parent->delete_tid, trans->sync_tid);
#endif
	if (h2ignore_deleted(info, parent))
		base = NULL;

	/*
	 * Update the parent's blockref table and propagate mirror_tid.
	 *
	 * NOTE! Children with modify_tid's beyond our flush point are
	 *	 considered to not exist for the purposes of updating the
	 *	 parent's blockref array.
	 *
	 * NOTE! SCAN1 has already put the parent in a modified state
	 *	 so if it isn't we panic.
	 *
	 * NOTE! chain->modify_tid vs chain->bref.modify_tid.  The chain's
	 *	 internal modify_tid is always updated based on creation
	 *	 or delete-duplicate.  However, the bref.modify_tid is NOT
	 *	 updated due to simple blockref updates.
	 */
#if FLUSH_DEBUG
	kprintf("chain %p->%p pass %d trans %016jx sync %p.%d %016jx/%d C=%016jx D=%016jx PMIRROR %016jx\n",
		parent, child,
		info->pass, trans->sync_tid,
		child, child->bref.type,
		child->bref.key, child->bref.keybits,
		child->modify_tid, child->delete_tid, parent->bref.mirror_tid);
#endif

	if (info->pass == 1 && child->delete_tid <= trans->sync_tid) {
		/*
		 * Deleting.  The block array is expected to contain the
		 * child's entry if:
		 *
		 * (1) The deletion occurred after the parent's block table
		 *     was last synchronized (delete_tid), and
		 *
		 * (2) The creation occurred before or during the parent's
		 *     last block table synchronization.
		 */
#if FLUSH_DEBUG
		kprintf("S2A %p.%d b=%p d/b=%016jx/%016jx m/b=%016jx/%016jx\n",
			child, child->bref.type,
			base, child->delete_tid, parent->bref.mirror_tid,
			child->modify_tid, parent->bref.mirror_tid);
#endif
		if (base &&
		    child->delete_tid > parent->bref.mirror_tid &&
		    child->modify_tid <= parent->bref.mirror_tid) {
			KKASSERT(child->flags & HAMMER2_CHAIN_MOVED);
			KKASSERT(parent->modify_tid == trans->sync_tid ||
				 (parent == &hmp->vchain ||
				  parent == &hmp->fchain));
			hammer2_rollup_stats(parent, child, -1);
			spin_lock(&above->cst.spin);
#if FLUSH_DEBUG
			kprintf("trans %jx parent %p.%d child %p.%d m/d %016jx/%016jx "
				"flg=%08x %016jx/%d delete\n",
				trans->sync_tid,
				parent, parent->bref.type,
				child, child->bref.type,
				child->modify_tid, child->delete_tid,
				child->flags,
				child->bref.key, child->bref.keybits);
#endif
			hammer2_base_delete(trans, parent, base, count,
					    &info->cache_index, child);
			spin_unlock(&above->cst.spin);
		}
	} else if (info->pass == 2 && child->delete_tid > trans->sync_tid) {
		/*
		 * Inserting.  The block array is expected to NOT contain
		 * the child's entry if:
		 *
		 * (1) The creation occurred after the parent's block table
		 *     was last synchronized (modify_tid), and
		 *
		 * (2) The child is not being deleted in the same
		 *     transaction.
		 */
#if FLUSH_DEBUG
		kprintf("S2B %p.%d b=%p d/b=%016jx/%016jx m/b=%016jx/%016jx\n",
			child, child->bref.type,
			base, child->delete_tid, parent->bref.mirror_tid,
			child->modify_tid, parent->bref.mirror_tid);
#endif
		if (base &&
		    child->modify_tid > parent->bref.mirror_tid) {
			KKASSERT(child->flags & HAMMER2_CHAIN_MOVED);
			KKASSERT(parent->modify_tid == trans->sync_tid ||
				 (parent == &hmp->vchain ||
				  parent == &hmp->fchain));
			hammer2_rollup_stats(parent, child, 1);
			spin_lock(&above->cst.spin);
#if FLUSH_DEBUG
			kprintf("trans %jx parent %p.%d child %p.%d m/d %016jx/%016jx "
				"flg=%08x %016jx/%d insert\n",
				trans->sync_tid,
				parent, parent->bref.type,
				child, child->bref.type,
				child->modify_tid, child->delete_tid,
				child->flags,
				child->bref.key, child->bref.keybits);
#endif
			hammer2_base_insert(trans, parent, base, count,
					    &info->cache_index, child);
			spin_unlock(&above->cst.spin);
		}
	} else if (info->pass == 3 &&
		   (child->delete_tid == HAMMER2_MAX_TID ||
		    child->delete_tid <= trans->sync_tid) &&
		   (child->flags & HAMMER2_CHAIN_MOVED)) {
		/*
		 * We can't clear the MOVED bit on children whos modify_tid
		 * is beyond our current trans (was tested at top of scan2),
		 * or on deleted children which have not yet been flushed
		 * (handled above).
		 *
		 * Scan all parents of this child and determine if any of
		 * them still need the child's MOVED bit.
		 */
		hammer2_chain_t *scan;

		if (hammer2_debug & 0x4000)
			kprintf("CHECKMOVED %p (parent=%p)", child, parent);

		ok = 1;
		spin_lock(&above->cst.spin);
		TAILQ_FOREACH(scan, &above->ownerq, core_entry) {
			/*
			 * Can't clear child's MOVED until all parent's have
			 * synchronized with it.
			 *
			 * Ignore deleted parents as-of this flush TID.
			 * Ignore the current parent being flushed.
			 */
			if (h2ignore_deleted(info, scan))
				continue;
			if (scan == parent)
				continue;

			/*
			 * For parents not already synchronized check to see
			 * if the flush has gotten past them yet or not.
			 */
			if (scan->bref.mirror_tid >= trans->sync_tid)
				continue;

			if (hammer2_debug & 0x4000) {
				kprintf("(fail scan %p %016jx/%016jx)",
					scan, scan->bref.mirror_tid,
					child->modify_tid);
			}
			ok = 0;
			break;
		}
		if (hammer2_debug & 0x4000)
			kprintf("\n");
		spin_unlock(&above->cst.spin);

		/*
		 * Can we finally clear MOVED?
		 */
		if (ok) {
			if (hammer2_debug & 0x4000)
				kprintf("clear moved %p.%d %016jx/%d\n",
					child, child->bref.type,
					child->bref.key, child->bref.keybits);
			atomic_clear_int(&child->flags, HAMMER2_CHAIN_MOVED);
			hammer2_chain_drop(child);	/* moved cleared */
			KKASSERT((child->flags & HAMMER2_CHAIN_MODIFIED) == 0);
		} else {
			if (hammer2_debug & 0x4000)
				kprintf("keep  moved %p.%d %016jx/%d\n",
					child, child->bref.type,
					child->bref.key, child->bref.keybits);
		}
	}

	/*
	 * Unlock the child.  This can wind up dropping the child's
	 * last ref, removing it from the parent's RB tree, and deallocating
	 * the structure.  The RB_SCAN() our caller is doing handles the
	 * situation.
	 */
	hammer2_chain_unlock(child);
	hammer2_chain_drop(child);
	spin_lock(&above->cst.spin);

	/*
	 * The parent may have been delete-duplicated.
	 */
	info->parent = parent;
finalize:
	return (0);
}

static
void
hammer2_rollup_stats(hammer2_chain_t *parent, hammer2_chain_t *child, int how)
{
#if 0
	hammer2_chain_t *grandp;
#endif

	parent->data_count += child->data_count;
	parent->inode_count += child->inode_count;
	child->data_count = 0;
	child->inode_count = 0;
	if (how < 0) {
		parent->data_count -= child->bytes;
		if (child->bref.type == HAMMER2_BREF_TYPE_INODE) {
			parent->inode_count -= 1;
#if 0
			/* XXX child->data may be NULL atm */
			parent->data_count -= child->data->ipdata.data_count;
			parent->inode_count -= child->data->ipdata.inode_count;
#endif
		}
	} else if (how > 0) {
		parent->data_count += child->bytes;
		if (child->bref.type == HAMMER2_BREF_TYPE_INODE) {
			parent->inode_count += 1;
#if 0
			/* XXX child->data may be NULL atm */
			parent->data_count += child->data->ipdata.data_count;
			parent->inode_count += child->data->ipdata.inode_count;
#endif
		}
	}
	if (parent->bref.type == HAMMER2_BREF_TYPE_INODE) {
		parent->data->ipdata.data_count += parent->data_count;
		parent->data->ipdata.inode_count += parent->inode_count;
#if 0
		for (grandp = parent->above->first_parent;
		     grandp;
		     grandp = grandp->next_parent) {
			grandp->data_count += parent->data_count;
			grandp->inode_count += parent->inode_count;
		}
#endif
		parent->data_count = 0;
		parent->inode_count = 0;
	}
}

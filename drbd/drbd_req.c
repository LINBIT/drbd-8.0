/*
-*- linux-c -*-
   drbd_req.c
   Kernel module for 2.6.x Kernels

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2001-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 1999-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <linux/autoconf.h>
#include <linux/module.h>

#include <linux/slab.h>
#include <linux/drbd.h>
#include "drbd_int.h"
#include "drbd_req.h"

/* outside of the ifdef
 * because of the _print_rq_state(,FIXME) in barrier_acked */
STATIC void _print_rq_state(struct drbd_request *req, const char *txt)
{
	const unsigned long s = req->rq_state;
	struct drbd_conf *mdev = req->mdev;
	const int rw = (req->master_bio == NULL ||
			bio_data_dir(req->master_bio) == WRITE) ?
		'W' : 'R';

	INFO("%s %p %c L%c%c%cN%c%c%c%c%c %u (%llus +%u) %s\n",
	     txt, req, rw,
	     s & RQ_LOCAL_PENDING ? 'p' : '-',
	     s & RQ_LOCAL_COMPLETED ? 'c' : '-',
	     s & RQ_LOCAL_OK ? 'o' : '-',
	     s & RQ_NET_PENDING ? 'p' : '-',
	     s & RQ_NET_QUEUED ? 'q' : '-',
	     s & RQ_NET_SENT ? 's' : '-',
	     s & RQ_NET_DONE ? 'd' : '-',
	     s & RQ_NET_OK ? 'o' : '-',
	     req->epoch,
	     (unsigned long long)req->sector,
	     req->size,
	     conns_to_name(mdev->state.conn));
}

/* #define VERBOSE_REQUEST_CODE */
#if defined(VERBOSE_REQUEST_CODE) || defined(ENABLE_DYNAMIC_TRACE)
STATIC void _print_req_mod(struct drbd_request *req, enum drbd_req_event what)
{
	struct drbd_conf *mdev = req->mdev;
	const int rw = (req->master_bio == NULL ||
			bio_data_dir(req->master_bio) == WRITE) ?
		'W' : 'R';

	static const char *rq_event_names[] = {
		[created] = "created",
		[to_be_send] = "to_be_send",
		[to_be_submitted] = "to_be_submitted",
		[queue_for_net_write] = "queue_for_net_write",
		[queue_for_net_read] = "queue_for_net_read",
		[send_canceled] = "send_canceled",
		[send_failed] = "send_failed",
		[handed_over_to_network] = "handed_over_to_network",
		[connection_lost_while_pending] =
					"connection_lost_while_pending",
		[recv_acked_by_peer] = "recv_acked_by_peer",
		[write_acked_by_peer] = "write_acked_by_peer",
		[neg_acked] = "neg_acked",
		[conflict_discarded_by_peer] = "conflict_discarded_by_peer",
		[barrier_acked] = "barrier_acked",
		[data_received] = "data_received",
		[read_completed_with_error] = "read_completed_with_error",
		[write_completed_with_error] = "write_completed_with_error",
		[completed_ok] = "completed_ok",
	};

	INFO("_req_mod(%p %c ,%s)\n", req, rw, rq_event_names[what]);
}

# ifdef ENABLE_DYNAMIC_TRACE
#  define print_rq_state(R, T) \
	MTRACE(TRACE_TYPE_RQ, TRACE_LVL_METRICS, _print_rq_state(R, T);)
#  define print_req_mod(T, W)  \
	MTRACE(TRACE_TYPE_RQ, TRACE_LVL_METRICS, _print_req_mod(T, W);)
# else
#  define print_rq_state(R, T) _print_rq_state(R, T)
#  define print_req_mod(T, W)  _print_req_mod(T, W)
# endif

#else
#define print_rq_state(R, T)
#define print_req_mod(T, W)
#endif

/* We only support diskstats for 2.6.16 and up.
 * see also commit commit a362357b6cd62643d4dda3b152639303d78473da
 * Author: Jens Axboe <axboe@suse.de>
 * Date:   Tue Nov 1 09:26:16 2005 +0100
 *     [BLOCK] Unify the seperate read/write io stat fields into arrays */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)
#define _drbd_start_io_acct(...) do {} while (0)
#define _drbd_end_io_acct(...)   do {} while (0)
#else

/* Update disk stats at start of I/O request */
static inline void _drbd_start_io_acct(struct drbd_conf *mdev, struct drbd_request *req, struct bio *bio)
{
	const int rw = bio_data_dir(bio);
#ifndef __disk_stat_inc
	int cpu;
#endif

	MUST_HOLD(&mdev->req_lock)
#ifdef __disk_stat_inc
	__disk_stat_inc(mdev->vdisk, ios[rw]);
	__disk_stat_add(mdev->vdisk, sectors[rw], bio_sectors(bio));
	disk_round_stats(mdev->vdisk);
	mdev->vdisk->in_flight++;
#else
	cpu = part_stat_lock();
	part_stat_inc(cpu, &mdev->vdisk->part0, ios[rw]);
	part_stat_add(cpu, &mdev->vdisk->part0, sectors[rw], bio_sectors(bio));
	part_stat_unlock();
	mdev->vdisk->part0.in_flight++;
#endif
}

/* Update disk stats when completing request upwards */
static inline void _drbd_end_io_acct(struct drbd_conf *mdev, struct drbd_request *req)
{
	int rw = bio_data_dir(req->master_bio);
	unsigned long duration = jiffies - req->start_time;
#ifndef __disk_stat_inc
	int cpu;
#endif

	MUST_HOLD(&mdev->req_lock)
#ifdef __disk_stat_add
	__disk_stat_add(mdev->vdisk, ticks[rw], duration);
	disk_round_stats(mdev->vdisk);
	mdev->vdisk->in_flight--;
#else
	cpu = part_stat_lock();
	part_stat_add(cpu, &mdev->vdisk->part0, ticks[rw], duration);
	part_round_stats(cpu, &mdev->vdisk->part0);
	part_stat_unlock();
	mdev->vdisk->part0.in_flight--;
#endif
}

#endif

static void _req_is_done(struct drbd_conf *mdev, struct drbd_request *req, const int rw)
{
	const unsigned long s = req->rq_state;
	/* if it was a write, we may have to set the corresponding
	 * bit(s) out-of-sync first. If it had a local part, we need to
	 * release the reference to the activity log. */
	if (rw == WRITE) {
		/* remove it from the transfer log.
		 * well, only if it had been there in the first
		 * place... if it had not (local only or conflicting
		 * and never sent), it should still be "empty" as
		 * initialised in drbd_req_new(), so we can list_del() it
		 * here unconditionally */
		list_del(&req->tl_requests);
		/* Set out-of-sync unless both OK flags are set
		 * (local only or remote failed).
		 * Other places where we set out-of-sync:
		 * READ with local io-error */
		if (!(s & RQ_NET_OK) || !(s & RQ_LOCAL_OK))
			drbd_set_out_of_sync(mdev, req->sector, req->size);

		if ((s & RQ_NET_OK) && (s & RQ_LOCAL_OK) && (s & RQ_NET_SIS))
			drbd_set_in_sync(mdev, req->sector, req->size);

		/* one might be tempted to move the drbd_al_complete_io
		 * to the local io completion callback drbd_endio_pri.
		 * but, if this was a mirror write, we may only
		 * drbd_al_complete_io after this is RQ_NET_DONE,
		 * otherwise the extent could be dropped from the al
		 * before it has actually been written on the peer.
		 * if we crash before our peer knows about the request,
		 * but after the extent has been dropped from the al,
		 * we would forget to resync the corresponding extent.
		 */
		if (s & RQ_LOCAL_MASK) {
			if (get_ldev_if_state(mdev, D_FAILED)) {
				drbd_al_complete_io(mdev, req->sector);
				put_ldev(mdev);
			} else if (DRBD_ratelimit(5*HZ,3)) {
				drbd_WARN("Should have called drbd_al_complete_io(, %llu), "
				     "but my Disk seems to have failed :(\n",
				     (unsigned long long) req->sector);
			}
		}
	}

	/* if it was a local io error, we want to notify our
	 * peer about that, and see if we need to
	 * detach the disk and stuff.
	 * to avoid allocating some special work
	 * struct, reuse the request. */

	/* THINK
	 * why do we do this not when we detect the error,
	 * but delay it until it is "done", i.e. possibly
	 * until the next barrier ack? */

	if (rw == WRITE &&
	    ((s & RQ_LOCAL_MASK) && !(s & RQ_LOCAL_OK))) {
		if (!(req->w.list.next == LIST_POISON1 ||
		      list_empty(&req->w.list))) {
			/* DEBUG ASSERT only; if this triggers, we
			 * probably corrupt the worker list here */
			DUMPP(req->w.list.next);
			DUMPP(req->w.list.prev);
		}
		req->w.cb = w_io_error;
		drbd_queue_work(&mdev->data.work, &req->w);
		/* drbd_req_free() is done in w_io_error */
	} else {
		drbd_req_free(req);
	}
}

static void queue_barrier(struct drbd_conf *mdev)
{
	struct drbd_tl_epoch *b;

	/* We are within the req_lock. Once we queued the barrier for sending,
	 * we set the CREATE_BARRIER bit. It is cleared as soon as a new
	 * barrier/epoch object is added. This is the only place this bit is
	 * set. It indicates that the barrier for this epoch is already queued,
	 * and no new epoch has been created yet. */
	if (test_bit(CREATE_BARRIER, &mdev->flags))
		return;

	b = mdev->newest_tle;
	b->w.cb = w_send_barrier;
	/* inc_ap_pending done here, so we won't
	 * get imbalanced on connection loss.
	 * dec_ap_pending will be done in got_BarrierAck
	 * or (on connection loss) in tl_clear.  */
	inc_ap_pending(mdev);
	drbd_queue_work(&mdev->data.work, &b->w);
	set_bit(CREATE_BARRIER, &mdev->flags);
}

static void _about_to_complete_local_write(struct drbd_conf *mdev,
	struct drbd_request *req)
{
	const unsigned long s = req->rq_state;
	struct drbd_request *i;
	struct drbd_epoch_entry *e;
	struct hlist_node *n;
	struct hlist_head *slot;

	/* before we can signal completion to the upper layers,
	 * we may need to close the current epoch */
	if (mdev->state.conn >= C_CONNECTED &&
	    req->epoch == mdev->newest_tle->br_number)
		queue_barrier(mdev);

	/* we need to do the conflict detection stuff,
	 * if we have the ee_hash (two_primaries) and
	 * this has been on the network */
	if ((s & RQ_NET_DONE) && mdev->ee_hash != NULL) {
		const sector_t sector = req->sector;
		const int size = req->size;

		/* ASSERT:
		 * there must be no conflicting requests, since
		 * they must have been failed on the spot */
#define OVERLAPS overlaps(sector, size, i->sector, i->size)
		slot = tl_hash_slot(mdev, sector);
		hlist_for_each_entry(i, n, slot, colision) {
			if (OVERLAPS) {
				ALERT("LOGIC BUG: completed: %p %llus +%u; "
				      "other: %p %llus +%u\n",
				      req, (unsigned long long)sector, size,
				      i, (unsigned long long)i->sector, i->size);
			}
		}

		/* maybe "wake" those conflicting epoch entries
		 * that wait for this request to finish.
		 *
		 * currently, there can be only _one_ such ee
		 * (well, or some more, which would be pending
		 * P_DISCARD_ACK not yet sent by the asender...),
		 * since we block the receiver thread upon the
		 * first conflict detection, which will wait on
		 * misc_wait.  maybe we want to assert that?
		 *
		 * anyways, if we found one,
		 * we just have to do a wake_up.  */
#undef OVERLAPS
#define OVERLAPS overlaps(sector, size, e->sector, e->size)
		slot = ee_hash_slot(mdev, req->sector);
		hlist_for_each_entry(e, n, slot, colision) {
			if (OVERLAPS) {
				wake_up(&mdev->misc_wait);
				break;
			}
		}
	}
#undef OVERLAPS
}

static void _complete_master_bio(struct drbd_conf *mdev,
	struct drbd_request *req, int error)
{
	dump_bio(mdev, req->master_bio, 1);
	bio_endio(req->master_bio, error);
	req->master_bio = NULL;
	dec_ap_bio(mdev);
}

void _req_may_be_done(struct drbd_request *req, int error)
{
	const unsigned long s = req->rq_state;
	struct drbd_conf *mdev = req->mdev;
	int rw;

	print_rq_state(req, "_req_may_be_done");
	MUST_HOLD(&mdev->req_lock)

	/* we must not complete the master bio, while it is
	 *	still being processed by _drbd_send_zc_bio (drbd_send_dblock)
	 *	not yet acknowledged by the peer
	 *	not yet completed by the local io subsystem
	 * these flags may get cleared in any order by
	 *	the worker,
	 *	the receiver,
	 *	the bio_endio completion callbacks.
	 */
	if (s & RQ_NET_QUEUED)
		return;
	if (s & RQ_NET_PENDING)
		return;
	if (s & RQ_LOCAL_PENDING)
		return;

	if (req->master_bio) {
		/* this is data_received (remote read)
		 * or protocol C P_WRITE_ACK
		 * or protocol B P_RECV_ACK
		 * or protocol A "handed_over_to_network" (SendAck)
		 * or canceled or failed,
		 * or killed from the transfer log due to connection loss.
		 */

		/*
		 * figure out whether to report success or failure.
		 *
		 * report success when at least one of the operations suceeded.
		 * or, to put the other way,
		 * only report failure, when both operations failed.
		 *
		 * what to do about the failures is handled elsewhere.
		 * what we need to do here is just: complete the master_bio.
		 */
		int ok = (s & RQ_LOCAL_OK) || (s & RQ_NET_OK);
		rw = bio_data_dir(req->master_bio);

		/* remove the request from the conflict detection
		 * respective block_id verification hash */
		if (!hlist_unhashed(&req->colision))
			hlist_del(&req->colision);
		else
			D_ASSERT((s & RQ_NET_MASK) == 0);

		/* for writes we need to do some extra housekeeping */
		if (rw == WRITE)
			_about_to_complete_local_write(mdev, req);

		/* FIXME not yet implemented...
		 * in case we got "suspended" (on_disconnect: freeze io)
		 * we may not yet complete the request...
		 * though, this is probably best handled elsewhere by not
		 * walking the transfer log until "unfreeze", so we won't end
		 * up here anyways during the freeze ...
		 * then again, if it is a READ, it is not in the TL at all.
		 * is it still leagal to complete a READ during freeze? */

		/* Update disk stats */
		_drbd_end_io_acct(mdev, req);

		_complete_master_bio(mdev, req,
				     ok ? 0 : (error ? error : -EIO));
	} else {
		/* only WRITE requests can end up here without a master_bio */
		rw = WRITE;
	}

	if ((s & RQ_NET_MASK) == 0 || (s & RQ_NET_DONE)) {
		/* this is disconnected (local only) operation,
		 * or protocol C P_WRITE_ACK,
		 * or protocol A or B P_BARRIER_ACK,
		 * or killed from the transfer log due to connection loss. */
		_req_is_done(mdev, req, rw);
	}
	/* else: network part and not DONE yet. that is
	 * protocol A or B, barrier ack still pending... */
}

/*
 * checks whether there was an overlapping request
 * or ee already registered.
 *
 * if so, return 1, in which case this request is completed on the spot,
 * without ever being submitted or send.
 *
 * return 0 if it is ok to submit this request.
 *
 * NOTE:
 * paranoia: assume something above us is broken, and issues different write
 * requests for the same block simultaneously...
 *
 * To ensure these won't be reordered differently on both nodes, resulting in
 * diverging data sets, we discard the later one(s). Not that this is supposed
 * to happen, but this is the rationale why we also have to check for
 * conflicting requests with local origin, and why we have to do so regardless
 * of whether we allowed multiple primaries.
 *
 * BTW, in case we only have one primary, the ee_hash is empty anyways, and the
 * second hlist_for_each_entry becomes a noop. This is even simpler than to
 * grab a reference on the net_conf, and check for the two_primaries flag...
 */
STATIC int _req_conflicts(struct drbd_request *req)
{
	struct drbd_conf *mdev = req->mdev;
	const sector_t sector = req->sector;
	const int size = req->size;
	struct drbd_request *i;
	struct drbd_epoch_entry *e;
	struct hlist_node *n;
	struct hlist_head *slot;

	MUST_HOLD(&mdev->req_lock);
	D_ASSERT(hlist_unhashed(&req->colision));

	/* FIXME should this get_net_conf/put_net_conf
	 * rather be done in drbd_make_request_common? */
	if (!get_net_conf(mdev))
		return 0;

	/* BUG_ON */
	ERR_IF (mdev->tl_hash_s == 0)
		goto out_no_conflict;
	BUG_ON(mdev->tl_hash == NULL);

#define OVERLAPS overlaps(i->sector, i->size, sector, size)
	slot = tl_hash_slot(mdev, sector);
	hlist_for_each_entry(i, n, slot, colision) {
		if (OVERLAPS) {
			ALERT("%s[%u] Concurrent local write detected! "
			      "[DISCARD L] new: %llus +%u; "
			      "pending: %llus +%u\n",
			      current->comm, current->pid,
			      (unsigned long long)sector, size,
			      (unsigned long long)i->sector, i->size);
			goto out_conflict;
		}
	}

	if (mdev->ee_hash_s) {
		/* now, check for overlapping requests with remote origin */
		BUG_ON(mdev->ee_hash == NULL);
#undef OVERLAPS
#define OVERLAPS overlaps(e->sector, e->size, sector, size)
		slot = ee_hash_slot(mdev, sector);
		hlist_for_each_entry(e, n, slot, colision) {
			if (OVERLAPS) {
				ALERT("%s[%u] Concurrent remote write detected!"
				      " [DISCARD L] new: %llus +%u; "
				      "pending: %llus +%u\n",
				      current->comm, current->pid,
				      (unsigned long long)sector, size,
				      (unsigned long long)e->sector, e->size);
				goto out_conflict;
			}
		}
	}
#undef OVERLAPS

out_no_conflict:
	/* this is like it should be, and what we expected.
	 * our users do behave after all... */
	put_net_conf(mdev);
	return 0;

out_conflict:
	put_net_conf(mdev);
	return 1;
}

/* obviously this could be coded as many single functions
 * instead of one huge switch,
 * or by putting the code directly in the respective locations
 * (as it has been before).
 *
 * but having it this way
 *  enforces that it is all in this one place, where it is easier to audit,
 *  it makes it obvious that whatever "event" "happens" to a request should
 *  happen "atomically" within the req_lock,
 *  and it enforces that we have to think in a very structured manner
 *  about the "events" that may happen to a request during its life time ...
 *
 * Though I think it is likely that we break this again into many
 * static inline void _req_mod_ ## what (req) ...
 */
void _req_mod(struct drbd_request *req, enum drbd_req_event what, int error)
{
	struct drbd_conf *mdev = req->mdev;
	MUST_HOLD(&mdev->req_lock);

	if (error && (bio_rw(req->master_bio) != READA))
		ERR("got an _req_mod() errno of %d\n", error);

	print_req_mod(req, what);

	switch (what) {
	default:
		ERR("LOGIC BUG in %s:%u\n", __FILE__ , __LINE__);
		return;

	/* does not happen...
	 * initialization done in drbd_req_new
	case created:
		break;
		*/

	case to_be_send: /* via network */
		/* reached via drbd_make_request_common
		 * and from FIXME w_read_retry_remote */
		D_ASSERT(!(req->rq_state & RQ_NET_MASK));
		req->rq_state |= RQ_NET_PENDING;
		inc_ap_pending(mdev);
		break;

	case to_be_submitted: /* locally */
		/* reached via drbd_make_request_common */
		D_ASSERT(!(req->rq_state & RQ_LOCAL_MASK));
		req->rq_state |= RQ_LOCAL_PENDING;
		break;

	/* FIXME these *_completed_* are basically the same.
	 * can probably be merged with some if (what == xy) */

	case completed_ok:
		if (bio_data_dir(req->private_bio) == WRITE)
			mdev->writ_cnt += req->size>>9;
		else
			mdev->read_cnt += req->size>>9;

		bio_put(req->private_bio);
		req->private_bio = NULL;

		req->rq_state |= (RQ_LOCAL_COMPLETED|RQ_LOCAL_OK);
		req->rq_state &= ~RQ_LOCAL_PENDING;

		_req_may_be_done(req, error);
		put_ldev(mdev);
		break;

	case write_completed_with_error:
		req->rq_state |= RQ_LOCAL_COMPLETED;
		req->rq_state &= ~RQ_LOCAL_PENDING;

		bio_put(req->private_bio);
		req->private_bio = NULL;
		ALERT("Local WRITE failed sec=%llus size=%u\n",
		      (unsigned long long)req->sector, req->size);
		/* and now: check how to handle local io error.
		 * FIXME see comment below in read_completed_with_error */
		__drbd_chk_io_error(mdev, FALSE);
		_req_may_be_done(req, error);
		put_ldev(mdev);
		break;

	case read_completed_with_error:
		if (bio_rw(req->master_bio) != READA)
			drbd_set_out_of_sync(mdev, req->sector, req->size);

		req->rq_state |= RQ_LOCAL_COMPLETED;
		req->rq_state &= ~RQ_LOCAL_PENDING;

		bio_put(req->private_bio);
		req->private_bio = NULL;
		if (bio_rw(req->master_bio) == READA) {
			/* it is legal to fail READA */
			_req_may_be_done(req, error);
			put_ldev(mdev);
			break;
		}
		/* else */
		ALERT("Local READ failed sec=%llus size=%u\n",
		      (unsigned long long)req->sector, req->size);
		/* _req_mod(req,to_be_send); oops, recursion in static inline */
		D_ASSERT(!(req->rq_state & RQ_NET_MASK));
		req->rq_state |= RQ_NET_PENDING;
		inc_ap_pending(mdev);

		/* and now: check how to handle local io error.
		 *
		 * FIXME we should not handle WRITE and READ io errors
		 * the same. When we retry the READ, and then write
		 * the answer, that might suceed because modern drives
		 * would relocate the sectors. We'd need to keep our
		 * private bio then, and round the offset and size so
		 * we get back enough data to be able to clear the bits again.
		 */
		__drbd_chk_io_error(mdev, FALSE);
		put_ldev(mdev);
		/* NOTE: if we have no connection,
		 * or know the peer has no good data either,
		 * then we don't actually need to "queue_for_net_read",
		 * but we do so anyways, since the drbd_io_error()
		 * and the potential state change to "Diskless"
		 * needs to be done from process context */

		/* fall through: _req_mod(req,queue_for_net_read); */

	case queue_for_net_read:
		/* READ or READA, and
		 * no local disk,
		 * or target area marked as invalid,
		 * or just got an io-error. */
		/* from drbd_make_request_common
		 * or from bio_endio during read io-error recovery */

		/* so we can verify the handle in the answer packet
		 * corresponding hlist_del is in _req_may_be_done() */
		hlist_add_head(&req->colision, ar_hash_slot(mdev, req->sector));

		set_bit(UNPLUG_REMOTE, &mdev->flags); /* why? */

		D_ASSERT(req->rq_state & RQ_NET_PENDING);
		req->rq_state |= RQ_NET_QUEUED;
		req->w.cb = (req->rq_state & RQ_LOCAL_MASK)
			? w_read_retry_remote
			: w_send_read_req;
		drbd_queue_work(&mdev->data.work, &req->w);
		break;

	case queue_for_net_write:
		/* assert something? */
		/* from drbd_make_request_common only */

		hlist_add_head(&req->colision, tl_hash_slot(mdev, req->sector));
		/* corresponding hlist_del is in _req_may_be_done() */

		/* NOTE
		 * In case the req ended up on the transfer log before being
		 * queued on the worker, it could lead to this request being
		 * missed during cleanup after connection loss.
		 * So we have to do both operations here,
		 * within the same lock that protects the transfer log.
		 *
		 * _req_add_to_epoch(req); this has to be after the
		 * _maybe_start_new_epoch(req); which happened in
		 * drbd_make_request_common, because we now may set the bit
		 * again ourselves to close the current epoch.
		 *
		 * Add req to the (now) current epoch (barrier). */

		/* see drbd_make_request_common,
		 * just after it grabs the req_lock */
		D_ASSERT(test_bit(CREATE_BARRIER, &mdev->flags) == 0);

		req->epoch = mdev->newest_tle->br_number;
		list_add_tail(&req->tl_requests,
				&mdev->newest_tle->requests);

		/* increment size of current epoch */
		mdev->newest_tle->n_req++;

		/* queue work item to send data */
		D_ASSERT(req->rq_state & RQ_NET_PENDING);
		req->rq_state |= RQ_NET_QUEUED;
		req->w.cb =  w_send_dblock;
		drbd_queue_work(&mdev->data.work, &req->w);

		/* close the epoch, in case it outgrew the limit */
		if (mdev->newest_tle->n_req >= mdev->net_conf->max_epoch_size)
			queue_barrier(mdev);

		break;

	/* FIXME
	 * to implement freeze-io,
	 * we may not finish the request just yet.
	 */
	case send_canceled:
		/* treat it the same */
	case send_failed:
		/* real cleanup will be done from tl_clear.  just update flags
		 * so it is no longer marked as on the worker queue */
		req->rq_state &= ~RQ_NET_QUEUED;
		/* if we did it right, tl_clear should be scheduled only after
		 * this, so this should not be necessary! */
		_req_may_be_done(req, error);
		break;

	case handed_over_to_network:
		/* assert something? */
		if (bio_data_dir(req->master_bio) == WRITE &&
		    mdev->net_conf->wire_protocol == DRBD_PROT_A) {
			/* this is what is dangerous about protocol A:
			 * pretend it was sucessfully written on the peer.
			 * FIXME in case we get a local io-error in
			 * protocol != C, we might want to defer comletion
			 * until we get the barrier ack, and send a P_NEG_ACK
			 * in case the other node had an io-error, too...
			 * That way we would at least not report "success"
			 * if it was not written at all. */
			if (req->rq_state & RQ_NET_PENDING) {
				dec_ap_pending(mdev);
				req->rq_state &= ~RQ_NET_PENDING;
				req->rq_state |= RQ_NET_OK;
			} /* else: neg-ack was faster... */
			/* it is still not yet RQ_NET_DONE until the
			 * corresponding epoch barrier got acked as well,
			 * so we know what to dirty on connection loss */
		}
		req->rq_state &= ~RQ_NET_QUEUED;
		req->rq_state |= RQ_NET_SENT;
		/* because _drbd_send_zc_bio could sleep, and may want to
		 * dereference the bio even after the "write_acked_by_peer" and
		 * "completed_ok" events came in, once we return from
		 * _drbd_send_zc_bio (drbd_send_dblock), we have to check
		 * whether it is done already, and end it.  */
		_req_may_be_done(req, error);
		break;

	case connection_lost_while_pending:
		/* transfer log cleanup after connection loss */
		/* assert something? */
		if (req->rq_state & RQ_NET_PENDING)
			dec_ap_pending(mdev);
		req->rq_state &= ~(RQ_NET_OK|RQ_NET_PENDING);
		req->rq_state |= RQ_NET_DONE;
		/* if it is still queued, we may not complete it here.
		 * it will be canceled soon.
		 * FIXME we should change the code so this can not happen. */
		if (!(req->rq_state & RQ_NET_QUEUED))
			_req_may_be_done(req, error);
		break;

	case write_acked_by_peer_and_sis:
		req->rq_state |= RQ_NET_SIS;
	case conflict_discarded_by_peer:
		/* for discarded conflicting writes of multiple primarys,
		 * there is no need to keep anything in the tl, potential
		 * node crashes are covered by the activity log. */
		req->rq_state |= RQ_NET_DONE;
		/* fall through */
	case write_acked_by_peer:
		/* protocol C; successfully written on peer.
		 * Nothing to do here.
		 * We want to keep the tl in place for all protocols, to cater
		 * for volatile write-back caches on lower level devices.
		 *
		 * A barrier request is expected to have forced all prior
		 * requests onto stable storage, so completion of a barrier
		 * request could set NET_DONE right here, and not wait for the
		 * P_BARRIER_ACK, but that is an unecessary optimisation. */

		/* this makes it effectively the same as for: */
	case recv_acked_by_peer:
		/* protocol B; pretends to be sucessfully written on peer.
		 * see also notes above in handed_over_to_network about
		 * protocol != C */
		req->rq_state |= RQ_NET_OK;
		D_ASSERT(req->rq_state & RQ_NET_PENDING);
		dec_ap_pending(mdev);
		req->rq_state &= ~RQ_NET_PENDING;
		_req_may_be_done(req, error);
		break;

	case neg_acked:
		/* assert something? */
		if (req->rq_state & RQ_NET_PENDING)
			dec_ap_pending(mdev);
		req->rq_state &= ~(RQ_NET_OK|RQ_NET_PENDING);
		/* FIXME THINK! is it DONE now, or is it not? */
		req->rq_state |= RQ_NET_DONE;
		_req_may_be_done(req, error);
		/* else: done by handed_over_to_network */
		break;

	case barrier_acked:
		if (req->rq_state & RQ_NET_PENDING) {
			/* barrier came in before all requests have been acked.
			 * this is bad, because if the connection is lost now,
			 * we won't be able to clean them up... */
			_print_rq_state(req,
				"FIXME (barrier_acked but pending)");
			list_move(&req->tl_requests, &mdev->out_of_sequence_requests);
		}
		D_ASSERT(req->rq_state & RQ_NET_SENT);
		req->rq_state |= RQ_NET_DONE;
		_req_may_be_done(req, error);
		break;

	case data_received:
		D_ASSERT(req->rq_state & RQ_NET_PENDING);
		dec_ap_pending(mdev);
		req->rq_state &= ~RQ_NET_PENDING;
		req->rq_state |= (RQ_NET_OK|RQ_NET_DONE);
		_req_may_be_done(req, error);
		break;
	};
}

/* we may do a local read if:
 * - we are consistent (of course),
 * - or we are generally inconsistent,
 *   BUT we are still/already IN SYNC for this area.
 *   since size may be bigger than BM_BLOCK_SIZE,
 *   we may need to check several bits.
 */
STATIC int drbd_may_do_local_read(struct drbd_conf *mdev, sector_t sector, int size)
{
	unsigned long sbnr, ebnr;
	sector_t esector, nr_sectors;

	if (mdev->state.disk == D_UP_TO_DATE)
		return 1;
	if (mdev->state.disk >= D_OUTDATED)
		return 0;
	if (mdev->state.disk <  D_INCONSISTENT)
		return 0;
	/* state.disk == D_INCONSISTENT   We will have a look at the BitMap */
	nr_sectors = drbd_get_capacity(mdev->this_bdev);
	esector = sector + (size >> 9) - 1;

	D_ASSERT(sector  < nr_sectors);
	D_ASSERT(esector < nr_sectors);

	sbnr = BM_SECT_TO_BIT(sector);
	ebnr = BM_SECT_TO_BIT(esector);

	return 0 == drbd_bm_count_bits(mdev, sbnr, ebnr);
}

/*
 * general note:
 * looking at the state (conn, disk, susp, pdsk) outside of the spinlock that
 * protects the state changes is inherently racy.
 *
 * FIXME verify this rationale why we may do so anyways:
 *
 * I think it "should" be like this:
 * as soon as we have a "ap_bio_cnt" reference we may test for "bad" states,
 * because the transition from "bad" to "good" states may only happen while no
 * application request is on the fly, so once we are positive about a "bad"
 * state, we know it won't get better during the lifetime of this request.
 *
 * In case we think we are ok, but "asynchronously" some interrupt or other
 * thread marks some operation as impossible, we are still ok, since we would
 * just try anyways, and then see that it does not work there and then.
 */

STATIC int drbd_make_request_common(struct drbd_conf *mdev, struct bio *bio)
{
	const int rw = bio_rw(bio);
	const int size = bio->bi_size;
	const sector_t sector = bio->bi_sector;
	struct drbd_tl_epoch *b = NULL;
	struct drbd_request *req;
	int local, remote;
	int err = -EIO;

	/* allocate outside of all locks; */
	req = drbd_req_new(mdev, bio);
	if (!req) {
		dec_ap_bio(mdev);
		/* only pass the error to the upper layers.
		 * if user cannot handle io errors, thats not our business. */
		ERR("could not kmalloc() req\n");
		bio_endio(bio, -ENOMEM);
		return 0;
	}

	dump_bio(mdev, bio, 0);

	local = get_ldev(mdev);
	if (!local) {
		bio_put(req->private_bio); /* or we get a bio leak */
		req->private_bio = NULL;
	}
	if (rw == WRITE) {
		remote = 1;
	} else {
		/* READ || READA */
		if (local) {
			if (!drbd_may_do_local_read(mdev, sector, size)) {
				/* we could kick the syncer to
				 * sync this extent asap, wait for
				 * it, then continue locally.
				 * Or just issue the request remotely.
				 */
				/* FIXME
				 * I think we have a RACE here. We request
				 * something from the peer, then later some
				 * write starts ...  and finished *before*
				 * the answer to the read comes in, because
				 * the ACK for the WRITE goes over
				 * meta-socket ...
				 * Maybe we need to properly lock reads
				 * against the syncer, too. But if we have
				 * some user issuing writes on an area that
				 * he has pending reads on, _he_ is really
				 * broke anyways, and would get "undefined
				 * results" on _any_ io stack, even just the
				 * local io stack.
				 */

				local = 0;
				bio_put(req->private_bio);
				req->private_bio = NULL;
				put_ldev(mdev);
			}
		}
		remote = !local && mdev->state.pdsk >= D_UP_TO_DATE;
	}

	/* If we have a disk, but a READA request is mapped to remote,
	 * we are R_PRIMARY, D_INCONSISTENT, SyncTarget.
	 * Just fail that READA request right here.
	 *
	 * THINK: maybe fail all READA when not local?
	 *        or make this configurable...
	 *        if network is slow, READA won't do any good.
	 */
	if (rw == READA && mdev->state.disk >= D_INCONSISTENT && !local) {
		err = -EWOULDBLOCK;
		goto fail_and_free_req;
	}

	/* For WRITES going to the local disk, grab a reference on the target
	 * extent.  This waits for any resync activity in the corresponding
	 * resync extent to finish, and, if necessary, pulls in the target
	 * extent into the activity log, which involves further disk io because
	 * of transactional on-disk meta data updates. */
	if (rw == WRITE && local)
		drbd_al_begin_io(mdev, sector);

	remote = remote && (mdev->state.pdsk == D_UP_TO_DATE ||
			    (mdev->state.pdsk == D_INCONSISTENT &&
			     mdev->state.conn >= C_CONNECTED));

	if (!(local || remote)) {
		ERR("IO ERROR: neither local nor remote disk\n");
		goto fail_free_complete;
	}

	/* For WRITE request, we have to make sure that we have an
	 * unused_spare_tle, in case we need to start a new epoch.
	 * I try to be smart and avoid to pre-allocate always "just in case",
	 * but there is a race between testing the bit and pointer outside the
	 * spinlock, and grabbing the spinlock.
	 * if we lost that race, we retry.  */
	if (rw == WRITE && remote &&
	    mdev->unused_spare_tle == NULL &&
	    test_bit(CREATE_BARRIER, &mdev->flags)) {
allocate_barrier:
		b = kmalloc(sizeof(struct drbd_tl_epoch), GFP_NOIO);
		if (!b) {
			ERR("Failed to alloc barrier.\n");
			err = -ENOMEM;
			goto fail_free_complete;
		}
	}

	/* GOOD, everything prepared, grab the spin_lock */
	spin_lock_irq(&mdev->req_lock);

	/* FIXME race with drbd_disconnect and tl_clear? */
	if (remote) {
		remote = (mdev->state.pdsk == D_UP_TO_DATE ||
			    (mdev->state.pdsk == D_INCONSISTENT &&
			     mdev->state.conn >= C_CONNECTED));
		if (!remote)
			drbd_WARN("lost connection while grabbing the req_lock!\n");
		if (!(local || remote)) {
			ERR("IO ERROR: neither local nor remote disk\n");
			spin_unlock_irq(&mdev->req_lock);
			goto fail_free_complete;
		}
	}

	if (b && mdev->unused_spare_tle == NULL) {
		mdev->unused_spare_tle = b;
		b = NULL;
	}
	if (rw == WRITE && remote &&
	    mdev->unused_spare_tle == NULL &&
	    test_bit(CREATE_BARRIER, &mdev->flags)) {
		/* someone closed the current epoch
		 * while we were grabbing the spinlock */
		spin_unlock_irq(&mdev->req_lock);
		goto allocate_barrier;
	}


	/* Update disk stats */
	_drbd_start_io_acct(mdev, req, bio);

	/* _maybe_start_new_epoch(mdev);
	 * If we need to generate a write barrier packet, we have to add the
	 * new epoch (barrier) object, and queue the barrier packet for sending,
	 * and queue the req's data after it _within the same lock_, otherwise
	 * we have race conditions were the reorder domains could be mixed up.
	 *
	 * Even read requests may start a new epoch and queue the corresponding
	 * barrier packet.  To get the write ordering right, we only have to
	 * make sure that, if this is a write request and it triggered a
	 * barrier packet, this request is queued within the same spinlock. */
	if (remote && mdev->unused_spare_tle &&
	    test_and_clear_bit(CREATE_BARRIER, &mdev->flags)) {
		_tl_add_barrier(mdev, mdev->unused_spare_tle);
		mdev->unused_spare_tle = NULL;
	} else {
		D_ASSERT(!(remote && rw == WRITE &&
			   test_bit(CREATE_BARRIER, &mdev->flags)));
	}

	/* NOTE
	 * Actually, 'local' may be wrong here already, since we may have failed
	 * to write to the meta data, and may become wrong anytime because of
	 * local io-error for some other request, which would lead to us
	 * "detaching" the local disk.
	 *
	 * 'remote' may become wrong any time because the network could fail.
	 *
	 * This is a harmless race condition, though, since it is handled
	 * correctly at the appropriate places; so it just deferres the failure
	 * of the respective operation.
	 */

	/* mark them early for readability.
	 * this just sets some state flags. */
	if (remote)
		_req_mod(req, to_be_send, 0);
	if (local)
		_req_mod(req, to_be_submitted, 0);

	/* check this request on the colison detection hash tables.
	 * if we have a conflict, just complete it here.
	 * THINK do we want to check reads, too? (I don't think so...) */
	if (rw == WRITE && _req_conflicts(req)) {
		/* this is a conflicting request.
		 * even though it may have been only _partially_
		 * overlapping with one of the currently pending requests,
		 * without even submitting or sending it, we will
		 * pretend that it was successfully served right now.
		 */
		if (local) {
			bio_put(req->private_bio);
			req->private_bio = NULL;
			drbd_al_complete_io(mdev, req->sector);
			put_ldev(mdev);
			local = 0;
		}
		if (remote)
			dec_ap_pending(mdev);
		_drbd_end_io_acct(mdev, req);
		/* THINK: do we want to fail it (-EIO), or pretend success? */
		bio_endio(req->master_bio, 0);
		req->master_bio = NULL;
		dec_ap_bio(mdev);
		drbd_req_free(req);
		remote = 0;
	}

	/* NOTE remote first: to get the concurrent write detection right,
	 * we must register the request before start of local IO.  */
	if (remote) {
		/* either WRITE and C_CONNECTED,
		 * or READ, and no local disk,
		 * or READ, but not in sync.
		 */
		if (rw == WRITE)
			_req_mod(req, queue_for_net_write, 0);
		else
			_req_mod(req, queue_for_net_read, 0);
	}
	spin_unlock_irq(&mdev->req_lock);
	kfree(b); /* if someone else has beaten us to it... */

	if (local) {
		req->private_bio->bi_bdev = mdev->ldev->backing_bdev;

		dump_internal_bio("Pri", mdev, req->private_bio, 0);

		if (FAULT_ACTIVE(mdev, rw == WRITE ? DRBD_FAULT_DT_WR
				     : rw == READ  ? DRBD_FAULT_DT_RD
				     :               DRBD_FAULT_DT_RA))
			bio_endio(req->private_bio, -EIO);
		else
			generic_make_request(req->private_bio);
	}

	/* we need to plug ALWAYS since we possibly need to kick lo_dev.
	 * we plug after submit, so we won't miss an unplug event */
	drbd_plug_device(mdev);

	return 0;

fail_free_complete:
	if (rw == WRITE && local)
		drbd_al_complete_io(mdev, sector);
fail_and_free_req:
	if (local) {
		bio_put(req->private_bio);
		req->private_bio = NULL;
		put_ldev(mdev);
	}
	bio_endio(bio, err);
	drbd_req_free(req);
	dec_ap_bio(mdev);
	kfree(b);

	return 0;
}

/* helper function for drbd_make_request
 * if we can determine just by the mdev (state) that this request will fail,
 * return 1
 * otherwise return 0
 */
static int drbd_fail_request_early(struct drbd_conf *mdev, int is_write)
{
	/* Unconfigured */
	if (mdev->state.conn == C_DISCONNECTING &&
	    mdev->state.disk == D_DISKLESS)
		return 1;

	if (mdev->state.role != R_PRIMARY &&
		(!allow_oos || is_write)) {
		if (DRBD_ratelimit(5*HZ, 5)) {
			ERR("Process %s[%u] tried to %s; "
			    "since we are not in Primary state, "
			    "we cannot allow this\n",
			    current->comm, current->pid,
			    is_write ? "WRITE" : "READ");
		}
		return 1;
	}

	/*
	 * Paranoia: we might have been primary, but sync target, or
	 * even diskless, then lost the connection.
	 * This should have been handled (panic? suspend?) somehwere
	 * else. But maybe it was not, so check again here.
	 * Caution: as long as we do not have a read/write lock on mdev,
	 * to serialize state changes, this is racy, since we may lose
	 * the connection *after* we test for the cstate.
	 */
	if (mdev->state.disk < D_UP_TO_DATE && mdev->state.pdsk < D_UP_TO_DATE) {
		if (DRBD_ratelimit(5*HZ, 5))
			ERR("Sorry, I have no access to good data anymore.\n");
		/*
		 * FIXME suspend, loop waiting on cstate wait?
		 */
		return 1;
	}

	return 0;
}

int drbd_make_request_26(struct request_queue *q, struct bio *bio)
{
	unsigned int s_enr, e_enr;
	struct drbd_conf *mdev = (struct drbd_conf *) q->queuedata;

	if (drbd_fail_request_early(mdev, bio_data_dir(bio) & WRITE)) {
		bio_endio(bio, -EPERM);
		return 0;
	}

	/* Reject barrier requests if we know the underlying device does
	 * not support them.
	 * XXX: Need to get this info from peer as well some how so we
	 * XXX: reject if EITHER side/data/metadata area does not support them.
	 *
	 * because of those XXX, this is not yet enabled,
	 * i.e. in drbd_init_set_defaults we set the NO_BARRIER_SUPP bit.
	 */
	if (unlikely(bio_barrier(bio) && test_bit(NO_BARRIER_SUPP, &mdev->flags))) {
		/* drbd_WARN("Rejecting barrier request as underlying device does not support\n"); */
		bio_endio(bio, -EOPNOTSUPP);
		return 0;
	}

	/*
	 * what we "blindly" assume:
	 */
	D_ASSERT(bio->bi_size > 0);
	D_ASSERT((bio->bi_size & 0x1ff) == 0);
	D_ASSERT(bio->bi_idx == 0);

	/* to make some things easier, force allignment of requests within the
	 * granularity of our hash tables */
	s_enr = bio->bi_sector >> HT_SHIFT;
	e_enr = (bio->bi_sector+(bio->bi_size>>9)-1) >> HT_SHIFT;

	if (likely(s_enr == e_enr)) {
		inc_ap_bio(mdev, 1);
		return drbd_make_request_common(mdev, bio);
	}

	/* can this bio be split generically?
	 * Maybe add our own split-arbitrary-bios function. */
	if (bio->bi_vcnt != 1 || bio->bi_idx != 0 || bio->bi_size > DRBD_MAX_SEGMENT_SIZE) {
		/* rather error out here than BUG in bio_split */
		ERR("bio would need to, but cannot, be split: "
		    "(vcnt=%u,idx=%u,size=%u,sector=%llu)\n",
		    bio->bi_vcnt, bio->bi_idx, bio->bi_size,
		    (unsigned long long)bio->bi_sector);
		bio_endio(bio, -EINVAL);
	} else {
		/* This bio crosses some boundary, so we have to split it. */
		struct bio_pair *bp;
		/* works for the "do not cross hash slot boundaries" case
		 * e.g. sector 262269, size 4096
		 * s_enr = 262269 >> 6 = 4097
		 * e_enr = (262269+8-1) >> 6 = 4098
		 * HT_SHIFT = 6
		 * sps = 64, mask = 63
		 * first_sectors = 64 - (262269 & 63) = 3
		 */
		const sector_t sect = bio->bi_sector;
		const int sps = 1 << HT_SHIFT; /* sectors per slot */
		const int mask = sps - 1;
		const sector_t first_sectors = sps - (sect & mask);
		bp = bio_split(bio,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
				bio_split_pool,
#endif
				first_sectors);

		/* we need to get a "reference count" (ap_bio_cnt)
		 * to avoid races with the disconnect/reconnect/suspend code.
		 * In case we need to split the bio here, we need to get two references
		 * atomically, otherwise we might deadlock when trying to submit the
		 * second one! */
		inc_ap_bio(mdev, 2);

		D_ASSERT(e_enr == s_enr + 1);

		drbd_make_request_common(mdev, &bp->bio1);
		drbd_make_request_common(mdev, &bp->bio2);
		bio_pair_release(bp);
	}
	return 0;
}

/* This is called by bio_add_page().  With this function we reduce
 * the number of BIOs that span over multiple AL_EXTENTs.
 *
 * we do the calculation within the lower 32bit of the byte offsets,
 * since we don't care for actual offset, but only check whether it
 * would cross "activity log extent" boundaries.
 *
 * As long as the BIO is emtpy we have to allow at least one bvec,
 * regardless of size and offset.  so the resulting bio may still
 * cross extent boundaries.  those are dealt with (bio_split) in
 * drbd_make_request_26.
 */
int drbd_merge_bvec(struct request_queue *q,
#ifdef HAVE_bvec_merge_data
		struct bvec_merge_data *bvm,
#else
		struct bio *bvm,
#endif
		struct bio_vec *bvec)
{
	struct drbd_conf *mdev = (struct drbd_conf *) q->queuedata;
	unsigned int bio_offset =
		(unsigned int)bvm->bi_sector << 9; /* 32 bit */
	unsigned int bio_size = bvm->bi_size;
	int limit, backing_limit;

	limit = DRBD_MAX_SEGMENT_SIZE
	      - ((bio_offset & (DRBD_MAX_SEGMENT_SIZE-1)) + bio_size);
	if (limit < 0)
		limit = 0;
	if (bio_size == 0) {
		if (limit <= bvec->bv_len)
			limit = bvec->bv_len;
	} else if (limit && get_ldev(mdev)) {
		struct request_queue * const b =
			mdev->ldev->backing_bdev->bd_disk->queue;
		if (b->merge_bvec_fn && mdev->ldev->dc.use_bmbv) {
			backing_limit = b->merge_bvec_fn(b, bvm, bvec);
			limit = min(limit, backing_limit);
		}
		put_ldev(mdev);
	}
	return limit;
}

/*
-*- linux-c -*-
   drbd_actlog.c
   Kernel module for 2.6.x Kernels

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2003-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 2003-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2003-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

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

#include <linux/slab.h>
#include <linux/drbd.h>
#include "drbd_int.h"

/* This is what I like so much about the linux kernel:
 * if you have a close look, you can almost always reuse code by someone else
 * ;)
 * this is mostly from drivers/md/md.c
 */
STATIC int _drbd_md_sync_page_io(struct drbd_conf *mdev,
				 struct drbd_backing_dev *bdev,
				 struct page *page, sector_t sector,
				 int rw, int size)
{
	struct bio *bio;
	struct drbd_md_io md_io;
	int ok;

	md_io.mdev = mdev;
	init_completion(&md_io.event);
	md_io.error = 0;

	if (rw == WRITE && !test_bit(MD_NO_BARRIER, &mdev->flags))
		rw |= (1<<BIO_RW_BARRIER);
#ifdef BIO_RW_UNPLUG
	/* see upstream commits
	 * 213d9417fec62ef4c3675621b9364a667954d4dd,
	 * 93dbb393503d53cd226e5e1f0088fe8f4dbaa2b8 */
	rw |= (1<<BIO_RW_SYNCIO) | (1<<BIO_RW_UNPLUG);
#else
	rw |= (1<<BIO_RW_SYNC);
#endif

 retry:
	bio = bio_alloc(GFP_NOIO, 1);
	bio->bi_bdev = bdev->md_bdev;
	bio->bi_sector = sector;
	ok = (bio_add_page(bio, page, size, 0) == size);
	if (!ok)
		goto out;
	bio->bi_private = &md_io;
	bio->bi_end_io = drbd_md_io_complete;
	bio->bi_rw = rw;

	dump_internal_bio("Md", mdev, bio, 0);

	if (FAULT_ACTIVE(mdev, (rw & WRITE) ? DRBD_FAULT_MD_WR : DRBD_FAULT_MD_RD))
		bio_endio(bio, -EIO);
	else
		submit_bio(rw, bio);
	wait_for_completion(&md_io.event);
	ok = bio_flagged(bio, BIO_UPTODATE) && md_io.error == 0;

	/* check for unsupported barrier op.
	 * would rather check on EOPNOTSUPP, but that is not reliable.
	 * don't try again for ANY return value != 0 */
	if (unlikely(bio_barrier(bio) && !ok)) {
		/* Try again with no barrier */
		drbd_WARN("Barriers not supported on meta data device - disabling\n");
		set_bit(MD_NO_BARRIER, &mdev->flags);
		rw &= ~(1 << BIO_RW_BARRIER);
		bio_put(bio);
		goto retry;
	}
 out:
	bio_put(bio);
	return ok;
}

int drbd_md_sync_page_io(struct drbd_conf *mdev, struct drbd_backing_dev *bdev,
			 sector_t sector, int rw)
{
	int hardsect, mask, ok;
	int offset = 0;
	struct page *iop = mdev->md_io_page;

	D_ASSERT(semaphore_is_locked(&mdev->md_io_mutex));

	if (!bdev->md_bdev) {
		if (DRBD_ratelimit(5*HZ, 5)) {
			ERR("bdev->md_bdev==NULL\n");
			dump_stack();
		}
		return 0;
	}

	hardsect = drbd_get_hardsect(bdev->md_bdev);
	if (hardsect == 0)
		hardsect = MD_HARDSECT;

	/* in case hardsect != 512 [ s390 only? ] */
	if (hardsect != MD_HARDSECT) {
		if (!mdev->md_io_tmpp) {
			struct page *page = alloc_page(GFP_NOIO);
			if (!page)
				return 0;

			drbd_WARN("Meta data's bdev hardsect = %d != %d\n",
			     hardsect, MD_HARDSECT);
			drbd_WARN("Workaround engaged (has performace impact).\n");

			mdev->md_io_tmpp = page;
		}

		mask = (hardsect / MD_HARDSECT) - 1;
		D_ASSERT(mask == 1 || mask == 3 || mask == 7);
		D_ASSERT(hardsect == (mask+1) * MD_HARDSECT);
		offset = sector & mask;
		sector = sector & ~mask;
		iop = mdev->md_io_tmpp;

		if (rw == WRITE) {
			void *p = page_address(mdev->md_io_page);
			void *hp = page_address(mdev->md_io_tmpp);

			ok = _drbd_md_sync_page_io(mdev, bdev, iop,
						   sector, READ, hardsect);

			if (unlikely(!ok)) {
				ERR("drbd_md_sync_page_io(,%llus,"
				    "READ [hardsect!=512]) failed!\n",
				    (unsigned long long)sector);
				return 0;
			}

			memcpy(hp + offset*MD_HARDSECT , p, MD_HARDSECT);
		}
	}

#if DUMP_MD >= 3
	INFO("%s [%d]:%s(,%llus,%s)\n",
	     current->comm, current->pid, __func__,
	     (unsigned long long)sector, rw ? "WRITE" : "READ");
#endif

	if (sector < drbd_md_first_sector(bdev) ||
	    sector > drbd_md_last_sector(bdev))
		ALERT("%s [%d]:%s(,%llus,%s) out of range md access!\n",
		     current->comm, current->pid, __func__,
		     (unsigned long long)sector, rw ? "WRITE" : "READ");

	ok = _drbd_md_sync_page_io(mdev, bdev, iop, sector, rw, hardsect);
	if (unlikely(!ok)) {
		ERR("drbd_md_sync_page_io(,%llus,%s) failed!\n",
		    (unsigned long long)sector, rw ? "WRITE" : "READ");
		return 0;
	}

	if (hardsect != MD_HARDSECT && rw == READ) {
		void *p = page_address(mdev->md_io_page);
		void *hp = page_address(mdev->md_io_tmpp);

		memcpy(p, hp + offset*MD_HARDSECT, MD_HARDSECT);
	}

	return ok;
}

/* We maintain a trivial check sum in our on disk activity log.
 * With that we can ensure correct operation even when the storage
 * device might do a partial (last) sector write while loosing power.
 */
struct __attribute__((packed)) al_transaction {
	u32       magic;
	u32       tr_number;
	/* u32       tr_generation; TODO */
	struct __attribute__((packed)) {
		u32 pos;
		u32 extent; } updates[1 + AL_EXTENTS_PT];
	u32       xor_sum;
};

struct update_odbm_work {
	struct drbd_work w;
	unsigned int enr;
};

struct update_al_work {
	struct drbd_work w;
	struct lc_element *al_ext;
	struct completion event;
	unsigned int enr;
	/* if old_enr != LC_FREE, write corresponding bitmap sector, too */
	unsigned int old_enr;
};

int w_al_write_transaction(struct drbd_conf *, struct drbd_work *, int);

static inline
struct lc_element *_al_get(struct drbd_conf *mdev, unsigned int enr)
{
	struct lc_element *al_ext;
	struct bm_extent  *bm_ext;
	unsigned long     al_flags = 0;

	spin_lock_irq(&mdev->al_lock);
	bm_ext = (struct bm_extent *)
		lc_find(mdev->resync, enr/AL_EXT_PER_BM_SECT);
	if (unlikely(bm_ext != NULL)) {
		if (test_bit(BME_NO_WRITES, &bm_ext->flags)) {
			spin_unlock_irq(&mdev->al_lock);
			return NULL;
		}
	}
	al_ext   = lc_get(mdev->act_log, enr);
	al_flags = mdev->act_log->flags;
	spin_unlock_irq(&mdev->al_lock);

	/*
	if (!al_ext) {
		if (al_flags & LC_STARVING)
			drbd_WARN("Have to wait for LRU element (AL too small?)\n");
		if (al_flags & LC_DIRTY)
			drbd_WARN("Ongoing AL update (AL device too slow?)\n");
	}
	*/

	return al_ext;
}

/* FIXME
 * this should be able to return failure when meta data update has failed.
 */
void drbd_al_begin_io(struct drbd_conf *mdev, sector_t sector)
{
	unsigned int enr = (sector >> (AL_EXTENT_SIZE_B-9));
	struct lc_element *al_ext;
	struct update_al_work al_work;

	D_ASSERT(atomic_read(&mdev->local_cnt) > 0);

	MTRACE(TRACE_TYPE_AL_EXTS, TRACE_LVL_METRICS,
	       INFO("al_begin_io( sec=%llus (al_enr=%u) (rs_enr=%d) )\n",
		    (unsigned long long) sector, enr,
		    (int)BM_SECT_TO_EXT(sector));
	       );

	wait_event(mdev->al_wait, (al_ext = _al_get(mdev, enr)));

	if (al_ext->lc_number != enr) {
		/* drbd_al_write_transaction(mdev,al_ext,enr);
		 * recurses into generic_make_request(), which
		 * disalows recursion, bios being serialized on the
		 * current->bio_tail list now.
		 * we have to delegate updates to the activity log
		 * to the worker thread. */
		init_completion(&al_work.event);
		al_work.al_ext = al_ext;
		al_work.enr = enr;
		al_work.old_enr = al_ext->lc_number;
		al_work.w.cb = w_al_write_transaction;
		drbd_queue_work_front(&mdev->data.work, &al_work.w);
		wait_for_completion(&al_work.event);

		mdev->al_writ_cnt++;

		/*
		DUMPI(al_ext->lc_number);
		DUMPI(mdev->act_log->new_number);
		*/
		spin_lock_irq(&mdev->al_lock);
		lc_changed(mdev->act_log, al_ext);
		spin_unlock_irq(&mdev->al_lock);
		wake_up(&mdev->al_wait);
	}
}

void drbd_al_complete_io(struct drbd_conf *mdev, sector_t sector)
{
	unsigned int enr = (sector >> (AL_EXTENT_SIZE_B-9));
	struct lc_element *extent;
	unsigned long flags;

	MTRACE(TRACE_TYPE_AL_EXTS, TRACE_LVL_METRICS,
	       INFO("al_complete_io( sec=%llus (al_enr=%u) (rs_enr=%d) )\n",
		    (unsigned long long) sector, enr,
		    (int)BM_SECT_TO_EXT(sector));
	       );

	spin_lock_irqsave(&mdev->al_lock, flags);

	extent = lc_find(mdev->act_log, enr);

	if (!extent) {
		spin_unlock_irqrestore(&mdev->al_lock, flags);
		ERR("al_complete_io() called on inactive extent %u\n", enr);
		return;
	}

	if (lc_put(mdev->act_log, extent) == 0)
		wake_up(&mdev->al_wait);

	spin_unlock_irqrestore(&mdev->al_lock, flags);
}

int
w_al_write_transaction(struct drbd_conf *mdev, struct drbd_work *w, int unused)
{
	struct update_al_work *aw = (struct update_al_work *)w;
	struct lc_element *updated = aw->al_ext;
	const unsigned int new_enr = aw->enr;
	const unsigned int evicted = aw->old_enr;

	struct al_transaction *buffer;
	sector_t sector;
	int i, n, mx;
	unsigned int extent_nr;
	u32 xor_sum = 0;

	if (!get_ldev(mdev)) {
		ERR("get_ldev() failed in w_al_write_transaction\n");
		complete(&((struct update_al_work *)w)->event);
		return 1;
	}
	/* do we have to do a bitmap write, first?
	 * TODO reduce maximum latency:
	 * submit both bios, then wait for both,
	 * instead of doing two synchronous sector writes. */
	if (mdev->state.conn < C_CONNECTED && evicted != LC_FREE)
		drbd_bm_write_sect(mdev, evicted/AL_EXT_PER_BM_SECT);

	down(&mdev->md_io_mutex); /* protects md_io_buffer, al_tr_cycle, ... */
	buffer = (struct al_transaction *)page_address(mdev->md_io_page);

	buffer->magic = __constant_cpu_to_be32(DRBD_MAGIC);
	buffer->tr_number = cpu_to_be32(mdev->al_tr_number);

	n = lc_index_of(mdev->act_log, updated);

	buffer->updates[0].pos = cpu_to_be32(n);
	buffer->updates[0].extent = cpu_to_be32(new_enr);

	xor_sum ^= new_enr;

	mx = min_t(int, AL_EXTENTS_PT,
		   mdev->act_log->nr_elements - mdev->al_tr_cycle);
	for (i = 0; i < mx; i++) {
		extent_nr = lc_entry(mdev->act_log,
				     mdev->al_tr_cycle+i)->lc_number;
		buffer->updates[i+1].pos = cpu_to_be32(mdev->al_tr_cycle+i);
		buffer->updates[i+1].extent = cpu_to_be32(extent_nr);
		xor_sum ^= extent_nr;
	}
	for (; i < AL_EXTENTS_PT; i++) {
		buffer->updates[i+1].pos = __constant_cpu_to_be32(-1);
		buffer->updates[i+1].extent = __constant_cpu_to_be32(LC_FREE);
		xor_sum ^= LC_FREE;
	}
	mdev->al_tr_cycle += AL_EXTENTS_PT;
	if (mdev->al_tr_cycle >= mdev->act_log->nr_elements)
		mdev->al_tr_cycle = 0;

	buffer->xor_sum = cpu_to_be32(xor_sum);

	sector =  mdev->ldev->md.md_offset
		+ mdev->ldev->md.al_offset + mdev->al_tr_pos;

	if (!drbd_md_sync_page_io(mdev, mdev->ldev, sector, WRITE)) {
		drbd_chk_io_error(mdev, 1, TRUE);
		drbd_io_error(mdev, TRUE);
	}

	if (++mdev->al_tr_pos >
	    div_ceil(mdev->act_log->nr_elements, AL_EXTENTS_PT))
		mdev->al_tr_pos = 0;

	D_ASSERT(mdev->al_tr_pos < MD_AL_MAX_SIZE);
	mdev->al_tr_number++;

	up(&mdev->md_io_mutex);

	complete(&((struct update_al_work *)w)->event);
	put_ldev(mdev);

	return 1;
}

/**
 * drbd_al_read_tr: Reads a single transaction record from the
 * on disk activity log.
 * Returns -1 on IO error, 0 on checksum error and 1 if it is a valid
 * record.
 */
STATIC int drbd_al_read_tr(struct drbd_conf *mdev,
			   struct drbd_backing_dev *bdev,
			   struct al_transaction *b,
			   int index)
{
	sector_t sector;
	int rv, i;
	u32 xor_sum = 0;

	sector = bdev->md.md_offset + bdev->md.al_offset + index;

	/* Dont process error normally,
	 * as this is done before disk is atached! */
	if (!drbd_md_sync_page_io(mdev, bdev, sector, READ))
		return -1;

	rv = (be32_to_cpu(b->magic) == DRBD_MAGIC);

	for (i = 0; i < AL_EXTENTS_PT + 1; i++)
		xor_sum ^= be32_to_cpu(b->updates[i].extent);
	rv &= (xor_sum == be32_to_cpu(b->xor_sum));

	return rv;
}

/**
 * drbd_al_read_log: Restores the activity log from its on disk
 * representation. Returns 1 on success, returns 0 when
 * reading the log failed due to IO errors.
 */
int drbd_al_read_log(struct drbd_conf *mdev, struct drbd_backing_dev *bdev)
{
	struct al_transaction *buffer;
	int i;
	int rv;
	int mx;
	int cnr;
	int active_extents = 0;
	int transactions = 0;
	int overflow = 0;
	int from = -1;
	int to = -1;
	u32 from_tnr = -1;
	u32 to_tnr = 0;

	mx = div_ceil(mdev->act_log->nr_elements, AL_EXTENTS_PT);

	/* lock out all other meta data io for now,
	 * and make sure the page is mapped.
	 */
	down(&mdev->md_io_mutex);
	buffer = page_address(mdev->md_io_page);

	/* Find the valid transaction in the log */
	for (i = 0; i <= mx; i++) {
		rv = drbd_al_read_tr(mdev, bdev, buffer, i);
		if (rv == 0)
			continue;
		if (rv == -1) {
			up(&mdev->md_io_mutex);
			return 0;
		}
		cnr = be32_to_cpu(buffer->tr_number);

		if (cnr == -1)
			overflow = 1;

		if (cnr < from_tnr && !overflow) {
			from = i;
			from_tnr = cnr;
		}
		if (cnr > to_tnr) {
			to = i;
			to_tnr = cnr;
		}
	}

	if (from == -1 || to == -1) {
		drbd_WARN("No usable activity log found.\n");

		up(&mdev->md_io_mutex);
		return 1;
	}

	/* Read the valid transactions.
	 * INFO("Reading from %d to %d.\n",from,to); */
	i = from;
	while (1) {
		int j, pos;
		unsigned int extent_nr;
		unsigned int trn;

		rv = drbd_al_read_tr(mdev, bdev, buffer, i);
		ERR_IF(rv == 0) goto cancel;
		if (rv == -1) {
			up(&mdev->md_io_mutex);
			return 0;
		}

		trn = be32_to_cpu(buffer->tr_number);

		spin_lock_irq(&mdev->al_lock);

		/* This loop runs backwards because in the cyclic
		   elements there might be an old version of the
		   updated element (in slot 0). So the element in slot 0
		   can overwrite old versions. */
		for (j = AL_EXTENTS_PT; j >= 0; j--) {
			pos = be32_to_cpu(buffer->updates[j].pos);
			extent_nr = be32_to_cpu(buffer->updates[j].extent);

			if (extent_nr == LC_FREE)
				continue;

			lc_set(mdev->act_log, extent_nr, pos);
			active_extents++;
		}
		spin_unlock_irq(&mdev->al_lock);

		transactions++;

cancel:
		if (i == to)
			break;
		i++;
		if (i > mx)
			i = 0;
	}

	mdev->al_tr_number = to_tnr+1;
	mdev->al_tr_pos = to;
	if (++mdev->al_tr_pos >
	    div_ceil(mdev->act_log->nr_elements, AL_EXTENTS_PT))
		mdev->al_tr_pos = 0;

	/* ok, we are done with it */
	up(&mdev->md_io_mutex);

	INFO("Found %d transactions (%d active extents) in activity log.\n",
	     transactions, active_extents);

	return 1;
}

struct drbd_atodb_wait {
	atomic_t           count;
	struct completion  io_done;
	struct drbd_conf   *mdev;
	int                error;
};

STATIC BIO_ENDIO_TYPE atodb_endio BIO_ENDIO_ARGS(struct bio *bio, int error)
{
	struct drbd_atodb_wait *wc = bio->bi_private;
	struct drbd_conf *mdev = wc->mdev;
	struct page *page;
	int uptodate = bio_flagged(bio, BIO_UPTODATE);

	BIO_ENDIO_FN_START;
	/* strange behaviour of some lower level drivers...
	 * fail the request by clearing the uptodate flag,
	 * but do not return any error?! */
	if (!error && !uptodate)
		error = -EIO;

	/* corresponding drbd_io_error is in drbd_al_to_on_disk_bm */
	drbd_chk_io_error(mdev, error, TRUE);
	if (error && wc->error == 0)
		wc->error = error;

	if (atomic_dec_and_test(&wc->count))
		complete(&wc->io_done);

	page = bio->bi_io_vec[0].bv_page;
	put_page(page);
	bio_put(bio);
	mdev->bm_writ_cnt++;
	put_ldev(mdev);

	BIO_ENDIO_FN_RETURN;
}

#define S2W(s)	((s)<<(BM_EXT_SIZE_B-BM_BLOCK_SIZE_B-LN2_BPL))
/* activity log to on disk bitmap -- prepare bio unless that sector
 * is already covered by previously prepared bios */
STATIC int atodb_prepare_unless_covered(struct drbd_conf *mdev,
					struct bio **bios,
					unsigned int enr,
					struct drbd_atodb_wait *wc) __must_hold(local)
{
	struct bio *bio;
	struct page *page;
	sector_t on_disk_sector = enr + mdev->ldev->md.md_offset
				      + mdev->ldev->md.bm_offset;
	unsigned int page_offset = PAGE_SIZE;
	int offset;
	int i = 0;
	int err = -ENOMEM;

	/* Check if that enr is already covered by an already created bio.
	 * Caution, bios[] is not NULL terminated,
	 * but only initialized to all NULL.
	 * For completely scattered activity log,
	 * the last invocation iterates over all bios,
	 * and finds the last NULL entry.
	 */
	while ((bio = bios[i])) {
		if (bio->bi_sector == on_disk_sector)
			return 0;
		i++;
	}
	/* bios[i] == NULL, the next not yet used slot */

	bio = bio_alloc(GFP_KERNEL, 1);
	if (bio == NULL)
		return -ENOMEM;

	if (i > 0) {
		const struct bio_vec *prev_bv = bios[i-1]->bi_io_vec;
		page_offset = prev_bv->bv_offset + prev_bv->bv_len;
		page = prev_bv->bv_page;
	}
	if (page_offset == PAGE_SIZE) {
		page = alloc_page(__GFP_HIGHMEM);
		if (page == NULL)
			goto out_bio_put;
		page_offset = 0;
	} else {
		get_page(page);
	}

	offset = S2W(enr);
	drbd_bm_get_lel(mdev, offset,
			min_t(size_t, S2W(1), drbd_bm_words(mdev) - offset),
			kmap(page) + page_offset);
	kunmap(page);

	bio->bi_private = wc;
	bio->bi_end_io = atodb_endio;
	bio->bi_bdev = mdev->ldev->md_bdev;
	bio->bi_sector = on_disk_sector;

	if (bio_add_page(bio, page, MD_HARDSECT, page_offset) != MD_HARDSECT)
		goto out_put_page;

	atomic_inc(&wc->count);
	/* we already know that we may do this...
	 * get_ldev_if_state(mdev,D_ATTACHING);
	 * just get the extra reference, so that the local_cnt reflects
	 * the number of pending IO requests DRBD at its backing device.
	 */
	atomic_inc(&mdev->local_cnt);

	bios[i] = bio;

	return 0;

out_put_page:
	err = -EINVAL;
	put_page(page);
out_bio_put:
	bio_put(bio);
	return err;
}

/**
 * drbd_al_to_on_disk_bm:
 * Writes the areas of the bitmap which are covered by the
 * currently active extents of the activity log.
 * called when we detach (unconfigure) local storage,
 * or when we go from R_PRIMARY to R_SECONDARY state.
 */
void drbd_al_to_on_disk_bm(struct drbd_conf *mdev)
{
	int i, nr_elements;
	unsigned int enr;
	struct bio **bios;
	struct drbd_atodb_wait wc;

	ERR_IF (!get_ldev_if_state(mdev, D_ATTACHING))
		return; /* sorry, I don't have any act_log etc... */

	wait_event(mdev->al_wait, lc_try_lock(mdev->act_log));

	nr_elements = mdev->act_log->nr_elements;

	bios = kzalloc(sizeof(struct bio *) * nr_elements, GFP_KERNEL);
	if (!bios)
		goto submit_one_by_one;

	atomic_set(&wc.count, 0);
	init_completion(&wc.io_done);
	wc.mdev = mdev;
	wc.error = 0;

	for (i = 0; i < nr_elements; i++) {
		enr = lc_entry(mdev->act_log, i)->lc_number;
		if (enr == LC_FREE)
			continue;
		/* next statement also does atomic_inc wc.count and local_cnt */
		if (atodb_prepare_unless_covered(mdev, bios,
						enr/AL_EXT_PER_BM_SECT,
						&wc))
			goto free_bios_submit_one_by_one;
	}

	/* unneccessary optimization? */
	lc_unlock(mdev->act_log);
	wake_up(&mdev->al_wait);

	/* all prepared, submit them */
	for (i = 0; i < nr_elements; i++) {
		if (bios[i] == NULL)
			break;
		if (FAULT_ACTIVE(mdev, DRBD_FAULT_MD_WR)) {
			bios[i]->bi_rw = WRITE;
			bio_endio(bios[i], -EIO);
		} else {
			submit_bio(WRITE, bios[i]);
		}
	}

	drbd_blk_run_queue(bdev_get_queue(mdev->ldev->md_bdev));

	/* always (try to) flush bitmap to stable storage */
	drbd_md_flush(mdev);

	/* In case we did not submit a single IO do not wait for
	 * them to complete. ( Because we would wait forever here. )
	 *
	 * In case we had IOs and they are already complete, there
	 * is not point in waiting anyways.
	 * Therefore this if () ... */
	if (atomic_read(&wc.count))
		wait_for_completion(&wc.io_done);

	put_ldev(mdev);

	if (wc.error)
		drbd_io_error(mdev, TRUE);
	kfree(bios);
	return;

 free_bios_submit_one_by_one:
	/* free everything by calling the endio callback directly. */
	for (i = 0; i < nr_elements && bios[i]; i++)
		bio_endio(bios[i], 0);
	kfree(bios);

 submit_one_by_one:
	drbd_WARN("Using the slow drbd_al_to_on_disk_bm()\n");

	for (i = 0; i < mdev->act_log->nr_elements; i++) {
		enr = lc_entry(mdev->act_log, i)->lc_number;
		if (enr == LC_FREE)
			continue;
		/* Really slow: if we have al-extents 16..19 active,
		 * sector 4 will be written four times! Synchronous! */
		drbd_bm_write_sect(mdev, enr/AL_EXT_PER_BM_SECT);
	}

	lc_unlock(mdev->act_log);
	wake_up(&mdev->al_wait);
	put_ldev(mdev);
}

/**
 * drbd_al_apply_to_bm: Sets the bits in the in-memory bitmap
 * which are described by the active extents of the activity log.
 */
void drbd_al_apply_to_bm(struct drbd_conf *mdev)
{
	unsigned int enr;
	unsigned long add = 0;
	char ppb[10];
	int i;

	wait_event(mdev->al_wait, lc_try_lock(mdev->act_log));

	for (i = 0; i < mdev->act_log->nr_elements; i++) {
		enr = lc_entry(mdev->act_log, i)->lc_number;
		if (enr == LC_FREE)
			continue;
		add += drbd_bm_ALe_set_all(mdev, enr);
	}

	lc_unlock(mdev->act_log);
	wake_up(&mdev->al_wait);

	INFO("Marked additional %s as out-of-sync based on AL.\n",
	     ppsize(ppb, Bit2KB(add)));
}

static inline int _try_lc_del(struct drbd_conf *mdev, struct lc_element *al_ext)
{
	int rv;

	spin_lock_irq(&mdev->al_lock);
	rv = (al_ext->refcnt == 0);
	if (likely(rv))
		lc_del(mdev->act_log, al_ext);
	spin_unlock_irq(&mdev->al_lock);

	MTRACE(TRACE_TYPE_AL_EXTS, TRACE_LVL_METRICS,
		if (unlikely(!rv))
			INFO("Waiting for extent in drbd_al_shrink()\n");
	       );

	return rv;
}

/**
 * drbd_al_shrink: Removes all active extents form the activity log.
 * (but does not write any transactions)
 * You need to lock mdev->act_log with lc_try_lock() / lc_unlock()
 */
void drbd_al_shrink(struct drbd_conf *mdev)
{
	struct lc_element *al_ext;
	int i;

	D_ASSERT(test_bit(__LC_DIRTY, &mdev->act_log->flags));

	for (i = 0; i < mdev->act_log->nr_elements; i++) {
		al_ext = lc_entry(mdev->act_log, i);
		if (al_ext->lc_number == LC_FREE)
			continue;
		wait_event(mdev->al_wait, _try_lc_del(mdev, al_ext));
	}

	wake_up(&mdev->al_wait);
}

STATIC int w_update_odbm(struct drbd_conf *mdev, struct drbd_work *w, int unused)
{
	struct update_odbm_work *udw = (struct update_odbm_work *)w;

	if (!get_ldev(mdev)) {
		if (DRBD_ratelimit(5*HZ, 5))
			drbd_WARN("Can not update on disk bitmap, local IO disabled.\n");
		return 1;
	}

	drbd_bm_write_sect(mdev, udw->enr);
	put_ldev(mdev);

	kfree(udw);

	if (drbd_bm_total_weight(mdev) <= mdev->rs_failed) {
		switch (mdev->state.conn) {
		case C_SYNC_SOURCE:  case C_SYNC_TARGET:
		case C_PAUSED_SYNC_S: case C_PAUSED_SYNC_T:
			drbd_resync_finished(mdev);
		default:
			/* nothing to do */
			break;
		}
	}
	drbd_bcast_sync_progress(mdev);

	return 1;
}


/* ATTENTION. The AL's extents are 4MB each, while the extents in the
 * resync LRU-cache are 16MB each.
 * The caller of this function has to hold an get_ldev() reference.
 *
 * TODO will be obsoleted once we have a caching lru of the on disk bitmap
 */
STATIC void drbd_try_clear_on_disk_bm(struct drbd_conf *mdev, sector_t sector,
				      int count, int success)
{
	struct bm_extent *ext;
	struct update_odbm_work *udw;

	unsigned int enr;

	MUST_HOLD(&mdev->al_lock);
	D_ASSERT(atomic_read(&mdev->local_cnt));

	/* I simply assume that a sector/size pair never crosses
	 * a 16 MB extent border. (Currently this is true...) */
	enr = BM_SECT_TO_EXT(sector);

	ext = (struct bm_extent *) lc_get(mdev->resync, enr);
	if (ext) {
		if (ext->lce.lc_number == enr) {
			if (success)
				ext->rs_left -= count;
			else
				ext->rs_failed += count;
			if (ext->rs_left < ext->rs_failed) {
				ERR("BAD! sector=%llus enr=%u rs_left=%d "
				    "rs_failed=%d count=%d\n",
				     (unsigned long long)sector,
				     ext->lce.lc_number, ext->rs_left,
				     ext->rs_failed, count);
				dump_stack();
				/* FIXME brrrgs. should never happen! */
				lc_put(mdev->resync, &ext->lce);
				drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
				return;
			}
		} else {
			/* Normally this element should be in the cache,
			 * since drbd_rs_begin_io() pulled it already in.
			 *
			 * But maybe an application write finished, and we set
			 * something outside the resync lru_cache in sync.
			 */
			int rs_left = drbd_bm_e_weight(mdev, enr);
			if (ext->flags != 0) {
				drbd_WARN("changing resync lce: %d[%u;%02lx]"
				     " -> %d[%u;00]\n",
				     ext->lce.lc_number, ext->rs_left,
				     ext->flags, enr, rs_left);
				ext->flags = 0;
			}
			if (ext->rs_failed) {
				drbd_WARN("Kicking resync_lru element enr=%u "
				     "out with rs_failed=%d\n",
				     ext->lce.lc_number, ext->rs_failed);
				set_bit(WRITE_BM_AFTER_RESYNC, &mdev->flags);
			}
			ext->rs_left = rs_left;
			ext->rs_failed = success ? 0 : count;
			lc_changed(mdev->resync, &ext->lce);
		}
		lc_put(mdev->resync, &ext->lce);
		/* no race, we are within the al_lock! */

		if (ext->rs_left == ext->rs_failed) {
			ext->rs_failed = 0;

			udw = kmalloc(sizeof(*udw), GFP_ATOMIC);
			if (udw) {
				udw->enr = ext->lce.lc_number;
				udw->w.cb = w_update_odbm;
				drbd_queue_work_front(&mdev->data.work, &udw->w);
			} else {
				drbd_WARN("Could not kmalloc an udw\n");
				set_bit(WRITE_BM_AFTER_RESYNC, &mdev->flags);
			}
		}
	} else {
		ERR("lc_get() failed! locked=%d/%d flags=%lu\n",
		    mdev->resync_locked,
		    mdev->resync->nr_elements,
		    mdev->resync->flags);
	}
}

/* clear the bit corresponding to the piece of storage in question:
 * size byte of data starting from sector.  Only clear a bits of the affected
 * one ore more _aligned_ BM_BLOCK_SIZE blocks.
 *
 * called by worker on C_SYNC_TARGET and receiver on SyncSource.
 *
 */
void __drbd_set_in_sync(struct drbd_conf *mdev, sector_t sector, int size,
		       const char *file, const unsigned int line)
{
	/* Is called from worker and receiver context _only_ */
	unsigned long sbnr, ebnr, lbnr;
	unsigned long count = 0;
	sector_t esector, nr_sectors;
	int wake_up = 0;
	unsigned long flags;

	if (size <= 0 || (size & 0x1ff) != 0 || size > DRBD_MAX_SEGMENT_SIZE) {
		ERR("drbd_set_in_sync: sector=%llus size=%d nonsense!\n",
				(unsigned long long)sector, size);
		return;
	}
	nr_sectors = drbd_get_capacity(mdev->this_bdev);
	esector = sector + (size >> 9) - 1;

	ERR_IF(sector >= nr_sectors) return;
	ERR_IF(esector >= nr_sectors) esector = (nr_sectors-1);

	lbnr = BM_SECT_TO_BIT(nr_sectors-1);

	/* we clear it (in sync).
	 * round up start sector, round down end sector.  we make sure we only
	 * clear full, alligned, BM_BLOCK_SIZE (4K) blocks */
	if (unlikely(esector < BM_SECT_PER_BIT-1))
		return;
	if (unlikely(esector == (nr_sectors-1)))
		ebnr = lbnr;
	else
		ebnr = BM_SECT_TO_BIT(esector - (BM_SECT_PER_BIT-1));
	sbnr = BM_SECT_TO_BIT(sector + BM_SECT_PER_BIT-1);

	MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_METRICS,
	       INFO("drbd_set_in_sync: sector=%llus size=%u sbnr=%lu ebnr=%lu\n",
		    (unsigned long long)sector, size, sbnr, ebnr);
	    );

	if (sbnr > ebnr)
		return;

	/*
	 * ok, (capacity & 7) != 0 sometimes, but who cares...
	 * we count rs_{total,left} in bits, not sectors.
	 */
	spin_lock_irqsave(&mdev->al_lock, flags);
	count = drbd_bm_clear_bits(mdev, sbnr, ebnr);
	if (count) {
		/* we need the lock for drbd_try_clear_on_disk_bm */
		if (jiffies - mdev->rs_mark_time > HZ*10) {
			/* should be roling marks,
			 * but we estimate only anyways. */
			if (mdev->rs_mark_left != drbd_bm_total_weight(mdev) &&
			    mdev->state.conn != C_PAUSED_SYNC_T &&
			    mdev->state.conn != C_PAUSED_SYNC_S) {
				mdev->rs_mark_time = jiffies;
				mdev->rs_mark_left = drbd_bm_total_weight(mdev);
			}
		}
		if (get_ldev(mdev)) {
			drbd_try_clear_on_disk_bm(mdev, sector, count, TRUE);
			put_ldev(mdev);
		}
		/* just wake_up unconditional now, various lc_chaged(),
		 * lc_put() in drbd_try_clear_on_disk_bm(). */
		wake_up = 1;
	}
	spin_unlock_irqrestore(&mdev->al_lock, flags);
	if (wake_up)
		wake_up(&mdev->al_wait);
}

/*
 * this is intended to set one request worth of data out of sync.
 * affects at least 1 bit,
 * and at most 1+DRBD_MAX_SEGMENT_SIZE/BM_BLOCK_SIZE bits.
 *
 * called by tl_clear and drbd_send_dblock (==drbd_make_request).
 * so this can be _any_ process.
 */
void __drbd_set_out_of_sync(struct drbd_conf *mdev, sector_t sector, int size,
			    const char *file, const unsigned int line)
{
	unsigned long sbnr, ebnr, lbnr;
	sector_t esector, nr_sectors;

	if (size <= 0 || (size & 0x1ff) != 0 || size > DRBD_MAX_SEGMENT_SIZE) {
		ERR("sector: %llus, size: %d\n",
			(unsigned long long)sector, size);
		return;
	}

	if (!get_ldev(mdev))
		return; /* no disk, no metadata, no bitmap to set bits in */

	nr_sectors = drbd_get_capacity(mdev->this_bdev);
	esector = sector + (size >> 9) - 1;

	ERR_IF(sector >= nr_sectors)
		goto out;
	ERR_IF(esector >= nr_sectors)
		esector = (nr_sectors-1);

	lbnr = BM_SECT_TO_BIT(nr_sectors-1);

	/* we set it out of sync,
	 * we do not need to round anything here */
	sbnr = BM_SECT_TO_BIT(sector);
	ebnr = BM_SECT_TO_BIT(esector);

	MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_METRICS,
	       INFO("drbd_set_out_of_sync: sector=%llus size=%u "
		    "sbnr=%lu ebnr=%lu\n",
		    (unsigned long long)sector, size, sbnr, ebnr);
	    );

	/* ok, (capacity & 7) != 0 sometimes, but who cares...
	 * we count rs_{total,left} in bits, not sectors.  */
	drbd_bm_set_bits(mdev, sbnr, ebnr);

out:
	put_ldev(mdev);
}

static inline
struct bm_extent *_bme_get(struct drbd_conf *mdev, unsigned int enr)
{
	struct bm_extent  *bm_ext;
	int wakeup = 0;
	unsigned long     rs_flags;

	spin_lock_irq(&mdev->al_lock);
	if (mdev->resync_locked > mdev->resync->nr_elements/2) {
		spin_unlock_irq(&mdev->al_lock);
		return NULL;
	}
	bm_ext = (struct bm_extent *) lc_get(mdev->resync, enr);
	if (bm_ext) {
		if (bm_ext->lce.lc_number != enr) {
			bm_ext->rs_left = drbd_bm_e_weight(mdev, enr);
			bm_ext->rs_failed = 0;
			lc_changed(mdev->resync, (struct lc_element *)bm_ext);
			wakeup = 1;
		}
		if (bm_ext->lce.refcnt == 1)
			mdev->resync_locked++;
		set_bit(BME_NO_WRITES, &bm_ext->flags);
	}
	rs_flags = mdev->resync->flags;
	spin_unlock_irq(&mdev->al_lock);
	if (wakeup)
		wake_up(&mdev->al_wait);

	if (!bm_ext) {
		if (rs_flags & LC_STARVING)
			drbd_WARN("Have to wait for element"
			     " (resync LRU too small?)\n");
		BUG_ON(rs_flags & LC_DIRTY);
	}

	return bm_ext;
}

static inline int _is_in_al(struct drbd_conf *mdev, unsigned int enr)
{
	struct lc_element *al_ext;
	int rv = 0;

	spin_lock_irq(&mdev->al_lock);
	if (unlikely(enr == mdev->act_log->new_number))
		rv = 1;
	else {
		al_ext = lc_find(mdev->act_log, enr);
		if (al_ext) {
			if (al_ext->refcnt)
				rv = 1;
		}
	}
	spin_unlock_irq(&mdev->al_lock);

	/*
	if (unlikely(rv)) {
		INFO("Delaying sync read until app's write is done\n");
	}
	*/
	return rv;
}

/**
 * drbd_rs_begin_io: Gets an extent in the resync LRU cache and sets it
 * to BME_LOCKED.
 *
 * @sector: The sector number
 *
 * sleeps on al_wait.
 * returns 1 if successful.
 * returns 0 if interrupted.
 */
int drbd_rs_begin_io(struct drbd_conf *mdev, sector_t sector)
{
	unsigned int enr = BM_SECT_TO_EXT(sector);
	struct bm_extent *bm_ext;
	int i, sig;

	MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_ALL,
	       INFO("drbd_rs_begin_io: sector=%llus (rs_end=%d)\n",
		    (unsigned long long)sector, enr);
	    );

	sig = wait_event_interruptible(mdev->al_wait,
			(bm_ext = _bme_get(mdev, enr)));
	if (sig)
		return 0;

	if (test_bit(BME_LOCKED, &bm_ext->flags))
		return 1;

	for (i = 0; i < AL_EXT_PER_BM_SECT; i++) {
		sig = wait_event_interruptible(mdev->al_wait,
				!_is_in_al(mdev, enr * AL_EXT_PER_BM_SECT + i));
		if (sig) {
			spin_lock_irq(&mdev->al_lock);
			if (lc_put(mdev->resync, &bm_ext->lce) == 0) {
				clear_bit(BME_NO_WRITES, &bm_ext->flags);
				mdev->resync_locked--;
				wake_up(&mdev->al_wait);
			}
			spin_unlock_irq(&mdev->al_lock);
			return 0;
		}
	}

	set_bit(BME_LOCKED, &bm_ext->flags);

	return 1;
}

/**
 * drbd_try_rs_begin_io: Gets an extent in the resync LRU cache, sets it
 * to BME_NO_WRITES, then tries to set it to BME_LOCKED.
 *
 * @sector: The sector number
 *
 * does not sleep.
 * returns zero if we could set BME_LOCKED and can proceed,
 * -EAGAIN if we need to try again.
 */
int drbd_try_rs_begin_io(struct drbd_conf *mdev, sector_t sector)
{
	unsigned int enr = BM_SECT_TO_EXT(sector);
	const unsigned int al_enr = enr*AL_EXT_PER_BM_SECT;
	struct bm_extent *bm_ext;
	int i;

	MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_ALL,
	       INFO("drbd_try_rs_begin_io: sector=%llus\n",
		    (unsigned long long)sector);
	    );

	spin_lock_irq(&mdev->al_lock);
	if (mdev->resync_wenr != LC_FREE && mdev->resync_wenr != enr) {
		/* in case you have very heavy scattered io, it may
		 * stall the syncer undefined if we giveup the ref count
		 * when we try again and requeue.
		 *
		 * if we don't give up the refcount, but the next time
		 * we are scheduled this extent has been "synced" by new
		 * application writes, we'd miss the lc_put on the
		 * extent we keept the refcount on.
		 * so we remembered which extent we had to try agin, and
		 * if the next requested one is something else, we do
		 * the lc_put here...
		 * we also have to wake_up
		 */
		MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_ALL,
			INFO("dropping %u, aparently got 'synced' "
			     "by application io\n", mdev->resync_wenr);
		);
		bm_ext = (struct bm_extent *)
			lc_find(mdev->resync, mdev->resync_wenr);
		if (bm_ext) {
			D_ASSERT(!test_bit(BME_LOCKED, &bm_ext->flags));
			D_ASSERT(test_bit(BME_NO_WRITES, &bm_ext->flags));
			clear_bit(BME_NO_WRITES, &bm_ext->flags);
			mdev->resync_wenr = LC_FREE;
			if (lc_put(mdev->resync, &bm_ext->lce) == 0)
				mdev->resync_locked--;
			wake_up(&mdev->al_wait);
		} else {
			ALERT("LOGIC BUG\n");
		}
	}
	bm_ext = (struct bm_extent *)lc_try_get(mdev->resync, enr);
	if (bm_ext) {
		if (test_bit(BME_LOCKED, &bm_ext->flags))
			goto proceed;
		if (!test_and_set_bit(BME_NO_WRITES, &bm_ext->flags)) {
			mdev->resync_locked++;
		} else {
			/* we did set the BME_NO_WRITES,
			 * but then could not set BME_LOCKED,
			 * so we tried again.
			 * drop the extra reference. */
			MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_ALL,
				INFO("dropping extra reference on %u\n", enr);
			);
			bm_ext->lce.refcnt--;
			D_ASSERT(bm_ext->lce.refcnt > 0);
		}
		goto check_al;
	} else {
		if (mdev->resync_locked > mdev->resync->nr_elements-3) {
			MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_ALL,
				INFO("resync_locked = %u!\n", mdev->resync_locked);
			);
			goto try_again;
		}
		bm_ext = (struct bm_extent *)lc_get(mdev->resync, enr);
		if (!bm_ext) {
			const unsigned long rs_flags = mdev->resync->flags;
			if (rs_flags & LC_STARVING)
				drbd_WARN("Have to wait for element"
				     " (resync LRU too small?)\n");
			BUG_ON(rs_flags & LC_DIRTY);
			goto try_again;
		}
		if (bm_ext->lce.lc_number != enr) {
			bm_ext->rs_left = drbd_bm_e_weight(mdev, enr);
			bm_ext->rs_failed = 0;
			lc_changed(mdev->resync, (struct lc_element *)bm_ext);
			wake_up(&mdev->al_wait);
			D_ASSERT(test_bit(BME_LOCKED, &bm_ext->flags) == 0);
		}
		set_bit(BME_NO_WRITES, &bm_ext->flags);
		D_ASSERT(bm_ext->lce.refcnt == 1);
		mdev->resync_locked++;
		goto check_al;
	}
check_al:
	MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_ALL,
		INFO("checking al for %u\n", enr);
	);
	for (i = 0; i < AL_EXT_PER_BM_SECT; i++) {
		if (unlikely(al_enr+i == mdev->act_log->new_number))
			goto try_again;
		if (lc_is_used(mdev->act_log, al_enr+i))
			goto try_again;
	}
	set_bit(BME_LOCKED, &bm_ext->flags);
proceed:
	mdev->resync_wenr = LC_FREE;
	spin_unlock_irq(&mdev->al_lock);
	return 0;

try_again:
	MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_ALL,
		INFO("need to try again for %u\n", enr);
	);
	if (bm_ext)
		mdev->resync_wenr = enr;
	spin_unlock_irq(&mdev->al_lock);
	return -EAGAIN;
}

void drbd_rs_complete_io(struct drbd_conf *mdev, sector_t sector)
{
	unsigned int enr = BM_SECT_TO_EXT(sector);
	struct bm_extent *bm_ext;
	unsigned long flags;

	MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_ALL,
	       INFO("drbd_rs_complete_io: sector=%llus (rs_enr=%d)\n",
		    (long long)sector, enr);
	    );

	spin_lock_irqsave(&mdev->al_lock, flags);
	bm_ext = (struct bm_extent *) lc_find(mdev->resync, enr);
	if (!bm_ext) {
		spin_unlock_irqrestore(&mdev->al_lock, flags);
		ERR("drbd_rs_complete_io() called, but extent not found\n");
		return;
	}

	if (bm_ext->lce.refcnt == 0) {
		spin_unlock_irqrestore(&mdev->al_lock, flags);
		ERR("drbd_rs_complete_io(,%llu [=%u]) called, "
		    "but refcnt is 0!?\n",
		    (unsigned long long)sector, enr);
		return;
	}

	if (lc_put(mdev->resync, (struct lc_element *)bm_ext) == 0) {
		clear_bit(BME_LOCKED, &bm_ext->flags);
		clear_bit(BME_NO_WRITES, &bm_ext->flags);
		mdev->resync_locked--;
		wake_up(&mdev->al_wait);
	}

	spin_unlock_irqrestore(&mdev->al_lock, flags);
}

/**
 * drbd_rs_cancel_all: Removes extents from the resync LRU. Even
 * if they are BME_LOCKED.
 */
void drbd_rs_cancel_all(struct drbd_conf *mdev)
{
	MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_METRICS,
	       INFO("drbd_rs_cancel_all\n");
	    );

	spin_lock_irq(&mdev->al_lock);

	if (get_ldev_if_state(mdev, D_FAILED)) { /* Makes sure ->resync is there. */
		lc_reset(mdev->resync);
		put_ldev(mdev);
	}
	mdev->resync_locked = 0;
	mdev->resync_wenr = LC_FREE;
	spin_unlock_irq(&mdev->al_lock);
	wake_up(&mdev->al_wait);
}

/**
 * drbd_rs_del_all: Gracefully remove all extents from the resync LRU.
 * there may be still a reference hold by someone. In that case this function
 * returns -EAGAIN.
 * In case all elements got removed it returns zero.
 */
int drbd_rs_del_all(struct drbd_conf *mdev)
{
	struct bm_extent *bm_ext;
	int i;

	MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_METRICS,
	       INFO("drbd_rs_del_all\n");
	    );

	spin_lock_irq(&mdev->al_lock);

	if (get_ldev_if_state(mdev, D_FAILED)) {
		/* ok, ->resync is there. */
		for (i = 0; i < mdev->resync->nr_elements; i++) {
			bm_ext = (struct bm_extent *) lc_entry(mdev->resync, i);
			if (bm_ext->lce.lc_number == LC_FREE)
				continue;
			if (bm_ext->lce.lc_number == mdev->resync_wenr) {
				INFO("dropping %u in drbd_rs_del_all, apparently"
				     " got 'synced' by application io\n",
				     mdev->resync_wenr);
				D_ASSERT(!test_bit(BME_LOCKED, &bm_ext->flags));
				D_ASSERT(test_bit(BME_NO_WRITES, &bm_ext->flags));
				clear_bit(BME_NO_WRITES, &bm_ext->flags);
				mdev->resync_wenr = LC_FREE;
				lc_put(mdev->resync, &bm_ext->lce);
			}
			if (bm_ext->lce.refcnt != 0) {
				INFO("Retrying drbd_rs_del_all() later. "
				     "refcnt=%d\n", bm_ext->lce.refcnt);
				put_ldev(mdev);
				spin_unlock_irq(&mdev->al_lock);
				return -EAGAIN;
			}
			D_ASSERT(bm_ext->rs_left == 0);
			D_ASSERT(!test_bit(BME_LOCKED, &bm_ext->flags));
			D_ASSERT(!test_bit(BME_NO_WRITES, &bm_ext->flags));
			lc_del(mdev->resync, &bm_ext->lce);
		}
		D_ASSERT(mdev->resync->used == 0);
		put_ldev(mdev);
	}
	spin_unlock_irq(&mdev->al_lock);

	return 0;
}

/* Record information on a failure to resync the specified blocks
 *
 * called on C_SYNC_TARGET when resync write fails or P_NEG_RS_DREPLY received
 *
 */
void drbd_rs_failed_io(struct drbd_conf *mdev, sector_t sector, int size)
{
	/* Is called from worker and receiver context _only_ */
	unsigned long sbnr, ebnr, lbnr;
	unsigned long count;
	sector_t esector, nr_sectors;
	int wake_up = 0;

	MTRACE(TRACE_TYPE_RESYNC, TRACE_LVL_SUMMARY,
	       INFO("drbd_rs_failed_io: sector=%llus, size=%u\n",
		    (unsigned long long)sector, size);
	    );

	if (size <= 0 || (size & 0x1ff) != 0 || size > DRBD_MAX_SEGMENT_SIZE) {
		ERR("drbd_rs_failed_io: sector=%llus size=%d nonsense!\n",
				(unsigned long long)sector, size);
		return;
	}
	nr_sectors = drbd_get_capacity(mdev->this_bdev);
	esector = sector + (size >> 9) - 1;

	ERR_IF(sector >= nr_sectors) return;
	ERR_IF(esector >= nr_sectors) esector = (nr_sectors-1);

	lbnr = BM_SECT_TO_BIT(nr_sectors-1);

	/*
	 * round up start sector, round down end sector.  we make sure we only
	 * handle full, alligned, BM_BLOCK_SIZE (4K) blocks */
	if (unlikely(esector < BM_SECT_PER_BIT-1))
		return;
	if (unlikely(esector == (nr_sectors-1)))
		ebnr = lbnr;
	else
		ebnr = BM_SECT_TO_BIT(esector - (BM_SECT_PER_BIT-1));
	sbnr = BM_SECT_TO_BIT(sector + BM_SECT_PER_BIT-1);

	if (sbnr > ebnr)
		return;

	/*
	 * ok, (capacity & 7) != 0 sometimes, but who cares...
	 * we count rs_{total,left} in bits, not sectors.
	 */
	spin_lock_irq(&mdev->al_lock);
	count = drbd_bm_count_bits(mdev, sbnr, ebnr);
	if (count) {
		mdev->rs_failed += count;

		if (get_ldev(mdev)) {
			drbd_try_clear_on_disk_bm(mdev, sector, count, FALSE);
			put_ldev(mdev);
		}

		/* just wake_up unconditional now, various lc_chaged(),
		 * lc_put() in drbd_try_clear_on_disk_bm(). */
		wake_up = 1;
	}
	spin_unlock_irq(&mdev->al_lock);
	if (wake_up)
		wake_up(&mdev->al_wait);
}

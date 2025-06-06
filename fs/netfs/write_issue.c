// SPDX-License-Identifier: GPL-2.0-only
/* Network filesystem high-level (buffered) writeback.
 *
 * Copyright (C) 2024 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 *
 * To support network filesystems with local caching, we manage a situation
 * that can be envisioned like the following:
 *
 *               +---+---+-----+-----+---+----------+
 *    Folios:    |   |   |     |     |   |          |
 *               +---+---+-----+-----+---+----------+
 *
 *                 +------+------+     +----+----+
 *    Upload:      |      |      |.....|    |    |
 *  (Stream 0)     +------+------+     +----+----+
 *
 *               +------+------+------+------+------+
 *    Cache:     |      |      |      |      |      |
 *  (Stream 1)   +------+------+------+------+------+
 *
 * Where we have a sequence of folios of varying sizes that we need to overlay
 * with multiple parallel streams of I/O requests, where the I/O requests in a
 * stream may also be of various sizes (in cifs, for example, the sizes are
 * negotiated with the server; in something like ceph, they may represent the
 * sizes of storage objects).
 *
 * The sequence in each stream may contain gaps and noncontiguous subrequests
 * may be glued together into single vectored write RPCs.
 */

#include <linux/export.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include "internal.h"

/*
 * Kill all dirty folios in the event of an unrecoverable error, starting with
 * a locked folio we've already obtained from writeback_iter().
 */
static void netfs_kill_dirty_pages(struct address_space *mapping,
				   struct writeback_control *wbc,
				   struct folio *folio)
{
	int error = 0;

	do {
		enum netfs_folio_trace why = netfs_folio_trace_kill;
		struct netfs_group *group = NULL;
		struct netfs_folio *finfo = NULL;
		void *priv;

		priv = folio_detach_private(folio);
		if (priv) {
			finfo = __netfs_folio_info(priv);
			if (finfo) {
				/* Kill folio from streaming write. */
				group = finfo->netfs_group;
				why = netfs_folio_trace_kill_s;
			} else {
				group = priv;
				if (group == NETFS_FOLIO_COPY_TO_CACHE) {
					/* Kill copy-to-cache folio */
					why = netfs_folio_trace_kill_cc;
					group = NULL;
				} else {
					/* Kill folio with group */
					why = netfs_folio_trace_kill_g;
				}
			}
		}

		trace_netfs_folio(folio, why);

		folio_start_writeback(folio);
		folio_unlock(folio);
		folio_end_writeback(folio);

		netfs_put_group(group);
		kfree(finfo);

	} while ((folio = writeback_iter(mapping, wbc, folio, &error)));
}

/*
 * Create a write request and set it up appropriately for the origin type.
 */
struct netfs_io_request *netfs_create_write_req(struct address_space *mapping,
						struct file *file,
						loff_t start,
						enum netfs_io_origin origin)
{
	struct netfs_io_request *wreq;
	struct netfs_inode *ictx;
	bool is_cacheable = (origin == NETFS_WRITEBACK ||
			     origin == NETFS_WRITEBACK_SINGLE ||
			     origin == NETFS_WRITETHROUGH ||
			     origin == NETFS_PGPRIV2_COPY_TO_CACHE);

	wreq = netfs_alloc_request(mapping, file, start, 0, origin);
	if (IS_ERR(wreq))
		return wreq;

	_enter("R=%x", wreq->debug_id);

	ictx = netfs_inode(wreq->inode);
	if (is_cacheable && netfs_is_cache_enabled(ictx))
		fscache_begin_write_operation(&wreq->cache_resources, netfs_i_cookie(ictx));
	if (rolling_buffer_init(&wreq->buffer, wreq->debug_id, ITER_SOURCE) < 0)
		goto nomem;

	wreq->cleaned_to = wreq->start;

	wreq->io_streams[0].stream_nr		= 0;
	wreq->io_streams[0].source		= NETFS_UPLOAD_TO_SERVER;
	wreq->io_streams[0].prepare_write	= ictx->ops->prepare_write;
	wreq->io_streams[0].issue_write		= ictx->ops->issue_write;
	wreq->io_streams[0].collected_to	= start;
	wreq->io_streams[0].transferred		= LONG_MAX;

	wreq->io_streams[1].stream_nr		= 1;
	wreq->io_streams[1].source		= NETFS_WRITE_TO_CACHE;
	wreq->io_streams[1].collected_to	= start;
	wreq->io_streams[1].transferred		= LONG_MAX;
	if (fscache_resources_valid(&wreq->cache_resources)) {
		wreq->io_streams[1].avail	= true;
		wreq->io_streams[1].active	= true;
		wreq->io_streams[1].prepare_write = wreq->cache_resources.ops->prepare_write_subreq;
		wreq->io_streams[1].issue_write = wreq->cache_resources.ops->issue_write;
	}

	return wreq;
nomem:
	wreq->error = -ENOMEM;
	netfs_put_request(wreq, netfs_rreq_trace_put_failed);
	return ERR_PTR(-ENOMEM);
}

/**
 * netfs_prepare_write_failed - Note write preparation failed
 * @subreq: The subrequest to mark
 *
 * Mark a subrequest to note that preparation for write failed.
 */
void netfs_prepare_write_failed(struct netfs_io_subrequest *subreq)
{
	__set_bit(NETFS_SREQ_FAILED, &subreq->flags);
	trace_netfs_sreq(subreq, netfs_sreq_trace_prep_failed);
}
EXPORT_SYMBOL(netfs_prepare_write_failed);

/*
 * Prepare a write subrequest.  We need to allocate a new subrequest
 * if we don't have one.
 */
static void netfs_prepare_write(struct netfs_io_request *wreq,
				struct netfs_io_stream *stream,
				loff_t start)
{
	struct netfs_io_subrequest *subreq;
	struct iov_iter *wreq_iter = &wreq->buffer.iter;

	/* Make sure we don't point the iterator at a used-up folio_queue
	 * struct being used as a placeholder to prevent the queue from
	 * collapsing.  In such a case, extend the queue.
	 */
	if (iov_iter_is_folioq(wreq_iter) &&
	    wreq_iter->folioq_slot >= folioq_nr_slots(wreq_iter->folioq))
		rolling_buffer_make_space(&wreq->buffer);

	subreq = netfs_alloc_subrequest(wreq);
	subreq->source		= stream->source;
	subreq->start		= start;
	subreq->stream_nr	= stream->stream_nr;
	subreq->io_iter		= *wreq_iter;

	_enter("R=%x[%x]", wreq->debug_id, subreq->debug_index);

	trace_netfs_sreq(subreq, netfs_sreq_trace_prepare);

	stream->sreq_max_len	= UINT_MAX;
	stream->sreq_max_segs	= INT_MAX;
	switch (stream->source) {
	case NETFS_UPLOAD_TO_SERVER:
		netfs_stat(&netfs_n_wh_upload);
		stream->sreq_max_len = wreq->wsize;
		break;
	case NETFS_WRITE_TO_CACHE:
		netfs_stat(&netfs_n_wh_write);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}

	if (stream->prepare_write)
		stream->prepare_write(subreq);

	__set_bit(NETFS_SREQ_IN_PROGRESS, &subreq->flags);

	/* We add to the end of the list whilst the collector may be walking
	 * the list.  The collector only goes nextwards and uses the lock to
	 * remove entries off of the front.
	 */
	spin_lock(&wreq->lock);
	list_add_tail(&subreq->rreq_link, &stream->subrequests);
	if (list_is_first(&subreq->rreq_link, &stream->subrequests)) {
		stream->front = subreq;
		if (!stream->active) {
			stream->collected_to = stream->front->start;
			/* Write list pointers before active flag */
			smp_store_release(&stream->active, true);
		}
	}

	spin_unlock(&wreq->lock);

	stream->construct = subreq;
}

/*
 * Set the I/O iterator for the filesystem/cache to use and dispatch the I/O
 * operation.  The operation may be asynchronous and should call
 * netfs_write_subrequest_terminated() when complete.
 */
static void netfs_do_issue_write(struct netfs_io_stream *stream,
				 struct netfs_io_subrequest *subreq)
{
	struct netfs_io_request *wreq = subreq->rreq;

	_enter("R=%x[%x],%zx", wreq->debug_id, subreq->debug_index, subreq->len);

	if (test_bit(NETFS_SREQ_FAILED, &subreq->flags))
		return netfs_write_subrequest_terminated(subreq, subreq->error);

	trace_netfs_sreq(subreq, netfs_sreq_trace_submit);
	stream->issue_write(subreq);
}

void netfs_reissue_write(struct netfs_io_stream *stream,
			 struct netfs_io_subrequest *subreq,
			 struct iov_iter *source)
{
	size_t size = subreq->len - subreq->transferred;

	// TODO: Use encrypted buffer
	subreq->io_iter = *source;
	iov_iter_advance(source, size);
	iov_iter_truncate(&subreq->io_iter, size);

	subreq->retry_count++;
	__clear_bit(NETFS_SREQ_MADE_PROGRESS, &subreq->flags);
	__set_bit(NETFS_SREQ_IN_PROGRESS, &subreq->flags);
	netfs_stat(&netfs_n_wh_retry_write_subreq);
	netfs_do_issue_write(stream, subreq);
}

void netfs_issue_write(struct netfs_io_request *wreq,
		       struct netfs_io_stream *stream)
{
	struct netfs_io_subrequest *subreq = stream->construct;

	if (!subreq)
		return;
	stream->construct = NULL;
	subreq->io_iter.count = subreq->len;
	netfs_do_issue_write(stream, subreq);
}

/*
 * Add data to the write subrequest, dispatching each as we fill it up or if it
 * is discontiguous with the previous.  We only fill one part at a time so that
 * we can avoid overrunning the credits obtained (cifs) and try to parallelise
 * content-crypto preparation with network writes.
 */
size_t netfs_advance_write(struct netfs_io_request *wreq,
			   struct netfs_io_stream *stream,
			   loff_t start, size_t len, bool to_eof)
{
	struct netfs_io_subrequest *subreq = stream->construct;
	size_t part;

	if (!stream->avail) {
		_leave("no write");
		return len;
	}

	_enter("R=%x[%x]", wreq->debug_id, subreq ? subreq->debug_index : 0);

	if (subreq && start != subreq->start + subreq->len) {
		netfs_issue_write(wreq, stream);
		subreq = NULL;
	}

	if (!stream->construct)
		netfs_prepare_write(wreq, stream, start);
	subreq = stream->construct;

	part = umin(stream->sreq_max_len - subreq->len, len);
	_debug("part %zx/%zx %zx/%zx", subreq->len, stream->sreq_max_len, part, len);
	subreq->len += part;
	subreq->nr_segs++;
	stream->submit_extendable_to -= part;

	if (subreq->len >= stream->sreq_max_len ||
	    subreq->nr_segs >= stream->sreq_max_segs ||
	    to_eof) {
		netfs_issue_write(wreq, stream);
		subreq = NULL;
	}

	return part;
}

/*
 * Write some of a pending folio data back to the server.
 */
static int netfs_write_folio(struct netfs_io_request *wreq,
			     struct writeback_control *wbc,
			     struct folio *folio)
{
	struct netfs_io_stream *upload = &wreq->io_streams[0];
	struct netfs_io_stream *cache  = &wreq->io_streams[1];
	struct netfs_io_stream *stream;
	struct netfs_group *fgroup; /* TODO: Use this with ceph */
	struct netfs_folio *finfo;
	size_t iter_off = 0;
	size_t fsize = folio_size(folio), flen = fsize, foff = 0;
	loff_t fpos = folio_pos(folio), i_size;
	bool to_eof = false, streamw = false;
	bool debug = false;

	_enter("");

	if (rolling_buffer_make_space(&wreq->buffer) < 0)
		return -ENOMEM;

	/* netfs_perform_write() may shift i_size around the page or from out
	 * of the page to beyond it, but cannot move i_size into or through the
	 * page since we have it locked.
	 */
	i_size = i_size_read(wreq->inode);

	if (fpos >= i_size) {
		/* mmap beyond eof. */
		_debug("beyond eof");
		folio_start_writeback(folio);
		folio_unlock(folio);
		wreq->nr_group_rel += netfs_folio_written_back(folio);
		netfs_put_group_many(wreq->group, wreq->nr_group_rel);
		wreq->nr_group_rel = 0;
		return 0;
	}

	if (fpos + fsize > wreq->i_size)
		wreq->i_size = i_size;

	fgroup = netfs_folio_group(folio);
	finfo = netfs_folio_info(folio);
	if (finfo) {
		foff = finfo->dirty_offset;
		flen = foff + finfo->dirty_len;
		streamw = true;
	}

	if (wreq->origin == NETFS_WRITETHROUGH) {
		to_eof = false;
		if (flen > i_size - fpos)
			flen = i_size - fpos;
	} else if (flen > i_size - fpos) {
		flen = i_size - fpos;
		if (!streamw)
			folio_zero_segment(folio, flen, fsize);
		to_eof = true;
	} else if (flen == i_size - fpos) {
		to_eof = true;
	}
	flen -= foff;

	_debug("folio %zx %zx %zx", foff, flen, fsize);

	/* Deal with discontinuities in the stream of dirty pages.  These can
	 * arise from a number of sources:
	 *
	 * (1) Intervening non-dirty pages from random-access writes, multiple
	 *     flushers writing back different parts simultaneously and manual
	 *     syncing.
	 *
	 * (2) Partially-written pages from write-streaming.
	 *
	 * (3) Pages that belong to a different write-back group (eg.  Ceph
	 *     snapshots).
	 *
	 * (4) Actually-clean pages that were marked for write to the cache
	 *     when they were read.  Note that these appear as a special
	 *     write-back group.
	 */
	if (fgroup == NETFS_FOLIO_COPY_TO_CACHE) {
		netfs_issue_write(wreq, upload);
	} else if (fgroup != wreq->group) {
		/* We can't write this page to the server yet. */
		kdebug("wrong group");
		folio_redirty_for_writepage(wbc, folio);
		folio_unlock(folio);
		netfs_issue_write(wreq, upload);
		netfs_issue_write(wreq, cache);
		return 0;
	}

	if (foff > 0)
		netfs_issue_write(wreq, upload);
	if (streamw)
		netfs_issue_write(wreq, cache);

	/* Flip the page to the writeback state and unlock.  If we're called
	 * from write-through, then the page has already been put into the wb
	 * state.
	 */
	if (wreq->origin == NETFS_WRITEBACK)
		folio_start_writeback(folio);
	folio_unlock(folio);

	if (fgroup == NETFS_FOLIO_COPY_TO_CACHE) {
		if (!cache->avail) {
			trace_netfs_folio(folio, netfs_folio_trace_cancel_copy);
			netfs_issue_write(wreq, upload);
			netfs_folio_written_back(folio);
			return 0;
		}
		trace_netfs_folio(folio, netfs_folio_trace_store_copy);
	} else if (!upload->avail && !cache->avail) {
		trace_netfs_folio(folio, netfs_folio_trace_cancel_store);
		netfs_folio_written_back(folio);
		return 0;
	} else if (!upload->construct) {
		trace_netfs_folio(folio, netfs_folio_trace_store);
	} else {
		trace_netfs_folio(folio, netfs_folio_trace_store_plus);
	}

	/* Attach the folio to the rolling buffer. */
	rolling_buffer_append(&wreq->buffer, folio, 0);

	/* Move the submission point forward to allow for write-streaming data
	 * not starting at the front of the page.  We don't do write-streaming
	 * with the cache as the cache requires DIO alignment.
	 *
	 * Also skip uploading for data that's been read and just needs copying
	 * to the cache.
	 */
	for (int s = 0; s < NR_IO_STREAMS; s++) {
		stream = &wreq->io_streams[s];
		stream->submit_off = foff;
		stream->submit_len = flen;
		if (!stream->avail ||
		    (stream->source == NETFS_WRITE_TO_CACHE && streamw) ||
		    (stream->source == NETFS_UPLOAD_TO_SERVER &&
		     fgroup == NETFS_FOLIO_COPY_TO_CACHE)) {
			stream->submit_off = UINT_MAX;
			stream->submit_len = 0;
		}
	}

	/* Attach the folio to one or more subrequests.  For a big folio, we
	 * could end up with thousands of subrequests if the wsize is small -
	 * but we might need to wait during the creation of subrequests for
	 * network resources (eg. SMB credits).
	 */
	for (;;) {
		ssize_t part;
		size_t lowest_off = ULONG_MAX;
		int choose_s = -1;

		/* Always add to the lowest-submitted stream first. */
		for (int s = 0; s < NR_IO_STREAMS; s++) {
			stream = &wreq->io_streams[s];
			if (stream->submit_len > 0 &&
			    stream->submit_off < lowest_off) {
				lowest_off = stream->submit_off;
				choose_s = s;
			}
		}

		if (choose_s < 0)
			break;
		stream = &wreq->io_streams[choose_s];

		/* Advance the iterator(s). */
		if (stream->submit_off > iter_off) {
			rolling_buffer_advance(&wreq->buffer, stream->submit_off - iter_off);
			iter_off = stream->submit_off;
		}

		atomic64_set(&wreq->issued_to, fpos + stream->submit_off);
		stream->submit_extendable_to = fsize - stream->submit_off;
		part = netfs_advance_write(wreq, stream, fpos + stream->submit_off,
					   stream->submit_len, to_eof);
		stream->submit_off += part;
		if (part > stream->submit_len)
			stream->submit_len = 0;
		else
			stream->submit_len -= part;
		if (part > 0)
			debug = true;
	}

	if (fsize > iter_off)
		rolling_buffer_advance(&wreq->buffer, fsize - iter_off);
	atomic64_set(&wreq->issued_to, fpos + fsize);

	if (!debug)
		kdebug("R=%x: No submit", wreq->debug_id);

	if (foff + flen < fsize)
		for (int s = 0; s < NR_IO_STREAMS; s++)
			netfs_issue_write(wreq, &wreq->io_streams[s]);

	_leave(" = 0");
	return 0;
}

/*
 * End the issuing of writes, letting the collector know we're done.
 */
static void netfs_end_issue_write(struct netfs_io_request *wreq)
{
	bool needs_poke = true;

	smp_wmb(); /* Write subreq lists before ALL_QUEUED. */
	set_bit(NETFS_RREQ_ALL_QUEUED, &wreq->flags);

	for (int s = 0; s < NR_IO_STREAMS; s++) {
		struct netfs_io_stream *stream = &wreq->io_streams[s];

		if (!stream->active)
			continue;
		if (!list_empty(&stream->subrequests))
			needs_poke = false;
		netfs_issue_write(wreq, stream);
	}

	if (needs_poke)
		netfs_wake_collector(wreq);
}

/*
 * Write some of the pending data back to the server
 */
int netfs_writepages(struct address_space *mapping,
		     struct writeback_control *wbc)
{
	struct netfs_inode *ictx = netfs_inode(mapping->host);
	struct netfs_io_request *wreq = NULL;
	struct folio *folio;
	int error = 0;

	if (!mutex_trylock(&ictx->wb_lock)) {
		if (wbc->sync_mode == WB_SYNC_NONE) {
			netfs_stat(&netfs_n_wb_lock_skip);
			return 0;
		}
		netfs_stat(&netfs_n_wb_lock_wait);
		mutex_lock(&ictx->wb_lock);
	}

	/* Need the first folio to be able to set up the op. */
	folio = writeback_iter(mapping, wbc, NULL, &error);
	if (!folio)
		goto out;

	wreq = netfs_create_write_req(mapping, NULL, folio_pos(folio), NETFS_WRITEBACK);
	if (IS_ERR(wreq)) {
		error = PTR_ERR(wreq);
		goto couldnt_start;
	}

	__set_bit(NETFS_RREQ_OFFLOAD_COLLECTION, &wreq->flags);
	trace_netfs_write(wreq, netfs_write_trace_writeback);
	netfs_stat(&netfs_n_wh_writepages);

	do {
		_debug("wbiter %lx %llx", folio->index, atomic64_read(&wreq->issued_to));

		/* It appears we don't have to handle cyclic writeback wrapping. */
		WARN_ON_ONCE(wreq && folio_pos(folio) < atomic64_read(&wreq->issued_to));

		if (netfs_folio_group(folio) != NETFS_FOLIO_COPY_TO_CACHE &&
		    unlikely(!test_bit(NETFS_RREQ_UPLOAD_TO_SERVER, &wreq->flags))) {
			set_bit(NETFS_RREQ_UPLOAD_TO_SERVER, &wreq->flags);
			wreq->netfs_ops->begin_writeback(wreq);
		}

		error = netfs_write_folio(wreq, wbc, folio);
		if (error < 0)
			break;
	} while ((folio = writeback_iter(mapping, wbc, folio, &error)));

	netfs_end_issue_write(wreq);

	mutex_unlock(&ictx->wb_lock);
	netfs_wake_collector(wreq);

	netfs_put_request(wreq, netfs_rreq_trace_put_return);
	_leave(" = %d", error);
	return error;

couldnt_start:
	netfs_kill_dirty_pages(mapping, wbc, folio);
out:
	mutex_unlock(&ictx->wb_lock);
	_leave(" = %d", error);
	return error;
}
EXPORT_SYMBOL(netfs_writepages);

/*
 * Begin a write operation for writing through the pagecache.
 */
struct netfs_io_request *netfs_begin_writethrough(struct kiocb *iocb, size_t len)
{
	struct netfs_io_request *wreq = NULL;
	struct netfs_inode *ictx = netfs_inode(file_inode(iocb->ki_filp));

	mutex_lock(&ictx->wb_lock);

	wreq = netfs_create_write_req(iocb->ki_filp->f_mapping, iocb->ki_filp,
				      iocb->ki_pos, NETFS_WRITETHROUGH);
	if (IS_ERR(wreq)) {
		mutex_unlock(&ictx->wb_lock);
		return wreq;
	}

	wreq->io_streams[0].avail = true;
	trace_netfs_write(wreq, netfs_write_trace_writethrough);
	return wreq;
}

/*
 * Advance the state of the write operation used when writing through the
 * pagecache.  Data has been copied into the pagecache that we need to append
 * to the request.  If we've added more than wsize then we need to create a new
 * subrequest.
 */
int netfs_advance_writethrough(struct netfs_io_request *wreq, struct writeback_control *wbc,
			       struct folio *folio, size_t copied, bool to_page_end,
			       struct folio **writethrough_cache)
{
	_enter("R=%x ic=%zu ws=%u cp=%zu tp=%u",
	       wreq->debug_id, wreq->buffer.iter.count, wreq->wsize, copied, to_page_end);

	if (!*writethrough_cache) {
		if (folio_test_dirty(folio))
			/* Sigh.  mmap. */
			folio_clear_dirty_for_io(folio);

		/* We can make multiple writes to the folio... */
		folio_start_writeback(folio);
		if (wreq->len == 0)
			trace_netfs_folio(folio, netfs_folio_trace_wthru);
		else
			trace_netfs_folio(folio, netfs_folio_trace_wthru_plus);
		*writethrough_cache = folio;
	}

	wreq->len += copied;
	if (!to_page_end)
		return 0;

	*writethrough_cache = NULL;
	return netfs_write_folio(wreq, wbc, folio);
}

/*
 * End a write operation used when writing through the pagecache.
 */
ssize_t netfs_end_writethrough(struct netfs_io_request *wreq, struct writeback_control *wbc,
			       struct folio *writethrough_cache)
{
	struct netfs_inode *ictx = netfs_inode(wreq->inode);
	ssize_t ret;

	_enter("R=%x", wreq->debug_id);

	if (writethrough_cache)
		netfs_write_folio(wreq, wbc, writethrough_cache);

	netfs_end_issue_write(wreq);

	mutex_unlock(&ictx->wb_lock);

	if (wreq->iocb)
		ret = -EIOCBQUEUED;
	else
		ret = netfs_wait_for_write(wreq);
	netfs_put_request(wreq, netfs_rreq_trace_put_return);
	return ret;
}

/*
 * Write data to the server without going through the pagecache and without
 * writing it to the local cache.
 */
int netfs_unbuffered_write(struct netfs_io_request *wreq, bool may_wait, size_t len)
{
	struct netfs_io_stream *upload = &wreq->io_streams[0];
	ssize_t part;
	loff_t start = wreq->start;
	int error = 0;

	_enter("%zx", len);

	if (wreq->origin == NETFS_DIO_WRITE)
		inode_dio_begin(wreq->inode);

	while (len) {
		// TODO: Prepare content encryption

		_debug("unbuffered %zx", len);
		part = netfs_advance_write(wreq, upload, start, len, false);
		start += part;
		len -= part;
		rolling_buffer_advance(&wreq->buffer, part);
		if (test_bit(NETFS_RREQ_PAUSE, &wreq->flags))
			netfs_wait_for_paused_write(wreq);
		if (test_bit(NETFS_RREQ_FAILED, &wreq->flags))
			break;
	}

	netfs_end_issue_write(wreq);
	_leave(" = %d", error);
	return error;
}

/*
 * Write some of a pending folio data back to the server and/or the cache.
 */
static int netfs_write_folio_single(struct netfs_io_request *wreq,
				    struct folio *folio)
{
	struct netfs_io_stream *upload = &wreq->io_streams[0];
	struct netfs_io_stream *cache  = &wreq->io_streams[1];
	struct netfs_io_stream *stream;
	size_t iter_off = 0;
	size_t fsize = folio_size(folio), flen;
	loff_t fpos = folio_pos(folio);
	bool to_eof = false;
	bool no_debug = false;

	_enter("");

	flen = folio_size(folio);
	if (flen > wreq->i_size - fpos) {
		flen = wreq->i_size - fpos;
		folio_zero_segment(folio, flen, fsize);
		to_eof = true;
	} else if (flen == wreq->i_size - fpos) {
		to_eof = true;
	}

	_debug("folio %zx/%zx", flen, fsize);

	if (!upload->avail && !cache->avail) {
		trace_netfs_folio(folio, netfs_folio_trace_cancel_store);
		return 0;
	}

	if (!upload->construct)
		trace_netfs_folio(folio, netfs_folio_trace_store);
	else
		trace_netfs_folio(folio, netfs_folio_trace_store_plus);

	/* Attach the folio to the rolling buffer. */
	folio_get(folio);
	rolling_buffer_append(&wreq->buffer, folio, NETFS_ROLLBUF_PUT_MARK);

	/* Move the submission point forward to allow for write-streaming data
	 * not starting at the front of the page.  We don't do write-streaming
	 * with the cache as the cache requires DIO alignment.
	 *
	 * Also skip uploading for data that's been read and just needs copying
	 * to the cache.
	 */
	for (int s = 0; s < NR_IO_STREAMS; s++) {
		stream = &wreq->io_streams[s];
		stream->submit_off = 0;
		stream->submit_len = flen;
		if (!stream->avail) {
			stream->submit_off = UINT_MAX;
			stream->submit_len = 0;
		}
	}

	/* Attach the folio to one or more subrequests.  For a big folio, we
	 * could end up with thousands of subrequests if the wsize is small -
	 * but we might need to wait during the creation of subrequests for
	 * network resources (eg. SMB credits).
	 */
	for (;;) {
		ssize_t part;
		size_t lowest_off = ULONG_MAX;
		int choose_s = -1;

		/* Always add to the lowest-submitted stream first. */
		for (int s = 0; s < NR_IO_STREAMS; s++) {
			stream = &wreq->io_streams[s];
			if (stream->submit_len > 0 &&
			    stream->submit_off < lowest_off) {
				lowest_off = stream->submit_off;
				choose_s = s;
			}
		}

		if (choose_s < 0)
			break;
		stream = &wreq->io_streams[choose_s];

		/* Advance the iterator(s). */
		if (stream->submit_off > iter_off) {
			rolling_buffer_advance(&wreq->buffer, stream->submit_off - iter_off);
			iter_off = stream->submit_off;
		}

		atomic64_set(&wreq->issued_to, fpos + stream->submit_off);
		stream->submit_extendable_to = fsize - stream->submit_off;
		part = netfs_advance_write(wreq, stream, fpos + stream->submit_off,
					   stream->submit_len, to_eof);
		stream->submit_off += part;
		if (part > stream->submit_len)
			stream->submit_len = 0;
		else
			stream->submit_len -= part;
		if (part > 0)
			no_debug = true;
	}

	wreq->buffer.iter.iov_offset = 0;
	if (fsize > iter_off)
		rolling_buffer_advance(&wreq->buffer, fsize - iter_off);
	atomic64_set(&wreq->issued_to, fpos + fsize);

	if (!no_debug)
		kdebug("R=%x: No submit", wreq->debug_id);
	_leave(" = 0");
	return 0;
}

/**
 * netfs_writeback_single - Write back a monolithic payload
 * @mapping: The mapping to write from
 * @wbc: Hints from the VM
 * @iter: Data to write, must be ITER_FOLIOQ.
 *
 * Write a monolithic, non-pagecache object back to the server and/or
 * the cache.
 */
int netfs_writeback_single(struct address_space *mapping,
			   struct writeback_control *wbc,
			   struct iov_iter *iter)
{
	struct netfs_io_request *wreq;
	struct netfs_inode *ictx = netfs_inode(mapping->host);
	struct folio_queue *fq;
	size_t size = iov_iter_count(iter);
	int ret;

	if (WARN_ON_ONCE(!iov_iter_is_folioq(iter)))
		return -EIO;

	if (!mutex_trylock(&ictx->wb_lock)) {
		if (wbc->sync_mode == WB_SYNC_NONE) {
			netfs_stat(&netfs_n_wb_lock_skip);
			return 0;
		}
		netfs_stat(&netfs_n_wb_lock_wait);
		mutex_lock(&ictx->wb_lock);
	}

	wreq = netfs_create_write_req(mapping, NULL, 0, NETFS_WRITEBACK_SINGLE);
	if (IS_ERR(wreq)) {
		ret = PTR_ERR(wreq);
		goto couldnt_start;
	}

	__set_bit(NETFS_RREQ_OFFLOAD_COLLECTION, &wreq->flags);
	trace_netfs_write(wreq, netfs_write_trace_writeback_single);
	netfs_stat(&netfs_n_wh_writepages);

	if (__test_and_set_bit(NETFS_RREQ_UPLOAD_TO_SERVER, &wreq->flags))
		wreq->netfs_ops->begin_writeback(wreq);

	for (fq = (struct folio_queue *)iter->folioq; fq; fq = fq->next) {
		for (int slot = 0; slot < folioq_count(fq); slot++) {
			struct folio *folio = folioq_folio(fq, slot);
			size_t part = umin(folioq_folio_size(fq, slot), size);

			_debug("wbiter %lx %llx", folio->index, atomic64_read(&wreq->issued_to));

			ret = netfs_write_folio_single(wreq, folio);
			if (ret < 0)
				goto stop;
			size -= part;
			if (size <= 0)
				goto stop;
		}
	}

stop:
	for (int s = 0; s < NR_IO_STREAMS; s++)
		netfs_issue_write(wreq, &wreq->io_streams[s]);
	smp_wmb(); /* Write lists before ALL_QUEUED. */
	set_bit(NETFS_RREQ_ALL_QUEUED, &wreq->flags);

	mutex_unlock(&ictx->wb_lock);
	netfs_wake_collector(wreq);

	netfs_put_request(wreq, netfs_rreq_trace_put_return);
	_leave(" = %d", ret);
	return ret;

couldnt_start:
	mutex_unlock(&ictx->wb_lock);
	_leave(" = %d", ret);
	return ret;
}
EXPORT_SYMBOL(netfs_writeback_single);

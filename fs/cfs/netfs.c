/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CFS - Ceph File System Simple
 *
 * Netfs framework support implementation
 * Provides I/O helpers for large file operations
 */

#include <linux/netfs.h>
#include <linux/ceph/osd_client.h>
#include <linux/slab.h>
#include <linux/uio.h>

#include "super.h"
#include "internal.h"
#include "rados.h"

/*
 * Default I/O sizes - can be overridden via mount options
 */
#ifndef CEPH_MAX_READ_SIZE
#define CEPH_MAX_READ_SIZE	(16 * 1024 * 1024)
#endif

#ifndef CEPH_MAX_WRITE_SIZE
#define CEPH_MAX_WRITE_SIZE	(16 * 1024 * 1024)
#endif

/*
 * Initialize a netfs I/O request
 */
static int cfs_netfs_init_request(struct netfs_io_request *rreq,
				  struct file *file)
{
	struct inode *inode = rreq->inode;
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_fs_info *fsi = CFS_SB(inode->i_sb);

	cfs_debug("netfs_init_request: ino=%llu, start=%llu, len=%zu\n",
		  ci->i_ino, rreq->start, rreq->len);

	/* Set up the netfs context for this inode */
	rreq->netfs_priv = fsi;
	
	/* Set max read/write sizes based on mount options */
	rreq->rsize = fsi->opts->rsize ?: CEPH_MAX_READ_SIZE;
	rreq->wsize = fsi->opts->wsize ?: CEPH_MAX_WRITE_SIZE;

	return 0;
}

/*
 * Free a netfs I/O request
 */
static void cfs_netfs_free_request(struct netfs_io_request *rreq)
{
	cfs_debug("netfs_free_request: done\n");
	/* Nothing special to clean up */
}

/*
 * Expand readahead request to optimize for RADOS
 */
static void cfs_netfs_expand_readahead(struct netfs_io_request *rreq)
{
	struct inode *inode = rreq->inode;
	struct cfs_inode_info *ci = CFS_I(inode);
	unsigned long ra_pages = inode->i_sb->s_bdi->ra_pages;
	loff_t end = rreq->start + rreq->len;
	loff_t new_end;

	cfs_debug("netfs_expand_readahead: ino=%llu, start=%llu, len=%zu\n",
		  ci->i_ino, rreq->start, rreq->len);

	/* Check if readahead is disabled */
	if (!ra_pages)
		return;

	/*
	 * Try to expand the readahead to align with 4MB blocks
	 * This reduces the number of OSD operations
	 */
	new_end = round_up(end, CFS_BLOCK_SIZE);
	if (new_end > rreq->i_size)
		new_end = rreq->i_size;
	
	if (new_end > end && new_end <= rreq->start + (ra_pages << PAGE_SHIFT))
		rreq->len = new_end - rreq->start;
}

/*
 * Clamp the length of a subrequest to fit within constraints
 * Returns true if the length was clamped
 */
static bool cfs_netfs_clamp_length(struct netfs_io_subrequest *subreq)
{
	struct netfs_io_request *rreq = subreq->rreq;
	struct inode *inode = rreq->inode;
	struct cfs_inode_info *ci = CFS_I(inode);
	u64 part_num, part_offset;
	size_t orig_len = subreq->len;

	/*
	 * Clamp to 4MB block boundaries to align with RADOS objects
	 */
	part_num = cfs_part_num(subreq->start);
	part_offset = cfs_part_offset(subreq->start);
	
	/* Adjust length to not exceed block boundary */
	subreq->len = min(subreq->len, CFS_BLOCK_SIZE - part_offset);
	
	/* Also limit by any max I/O size if set */
	subreq->len = min(subreq->len, (size_t)rreq->rsize);

	cfs_debug("netfs_clamp_length: ino=%llu, start=%llu, len=%zu -> %zu\n",
		  ci->i_ino, subreq->start, orig_len, subreq->len);

	return true;
}

/*
 * Issue a read operation to RADOS
 * This is the core function that actually performs the I/O
 */
static void cfs_netfs_issue_read(struct netfs_io_subrequest *subreq)
{
	struct netfs_io_request *rreq = subreq->rreq;
	struct inode *inode = rreq->inode;
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_fs_info *fsi = CFS_SB(inode->i_sb);
	struct ceph_object_id oid;
	struct page **pages;
	size_t len = subreq->len;
	size_t off = subreq->start;
	u32 part_num;
	size_t page_off;
	int ret;

	cfs_debug("netfs_issue_read: ino=%llu, start=%llu, len=%zu\n",
		  ci->i_ino, off, len);

	/* Calculate which data object to read from */
	part_num = cfs_part_num(off);
	cfs_data_oid(&oid, ci->i_ino, part_num);

	/* Get pages from the iterator using proper kernel API */
	ret = iov_iter_get_pages_alloc2(&subreq->io_iter, &pages, len, &page_off);
	if (ret < 0) {
		cfs_err("netfs_issue_read: failed to get pages: %d\n", ret);
		netfs_subreq_terminated(subreq, ret, false);
		return;
	}

	/* Should always give us page-aligned reads */
	WARN_ON_ONCE(page_off);
	len = ret;

	/* Issue the read to RADOS */
	ret = cfs_data_read(fsi, &oid, cfs_part_offset(off),
			   len, pages, DIV_ROUND_UP(len, PAGE_SIZE));
	
	kfree(pages);

	if (ret < 0) {
		cfs_err("netfs_issue_read: failed to read from RADOS: %d\n", ret);
		netfs_subreq_terminated(subreq, ret, false);
		return;
	}

	netfs_subreq_terminated(subreq, len, false);
}

/*
 * Issue a write operation to RADOS
 */
static void cfs_netfs_issue_write(struct netfs_io_subrequest *subreq)
{
	struct netfs_io_request *rreq = subreq->rreq;
	struct inode *inode = rreq->inode;
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_fs_info *fsi = CFS_SB(inode->i_sb);
	struct ceph_object_id oid;
	struct page **pages;
	size_t len = subreq->len;
	size_t off = subreq->start;
	u32 part_num;
	size_t page_off;
	int ret;

	cfs_debug("netfs_issue_write: ino=%llu, start=%llu, len=%zu\n",
		  ci->i_ino, off, len);

	/* Calculate which data object to write to */
	part_num = cfs_part_num(off);
	cfs_data_oid(&oid, ci->i_ino, part_num);

	/* Get pages from the iterator using proper kernel API */
	ret = iov_iter_get_pages_alloc2(&subreq->io_iter, &pages, len, &page_off);
	if (ret < 0) {
		cfs_err("netfs_issue_write: failed to get pages: %d\n", ret);
		netfs_subreq_terminated(subreq, ret, false);
		return;
	}

	/* Should always give us page-aligned writes */
	WARN_ON_ONCE(page_off);
	len = ret;

	/* Issue the write to RADOS */
	ret = cfs_data_write(fsi, &oid, cfs_part_offset(off),
			    len, pages, DIV_ROUND_UP(len, PAGE_SIZE));

	kfree(pages);

	if (ret < 0) {
		cfs_err("netfs_issue_write: failed to write to RADOS: %d\n", ret);
		netfs_subreq_terminated(subreq, ret, false);
		return;
	}

	netfs_subreq_terminated(subreq, len, false);
}

/*
 * Check if the data is still valid
 */
static bool cfs_netfs_is_still_valid(struct netfs_io_request *rreq)
{
	/* For RADOS, data is always authoritative */
	return true;
}

/*
 * Handle write begin - prepare for write
 */
static int cfs_netfs_check_write_begin(struct file *file, loff_t pos,
					unsigned int len,
					struct folio **foliop, void **_fsdata)
{
	/*
	 * Return 0 to use the default netfs implementation.
	 * Return -ENODATA to indicate we handled it ourselves.
	 * CFS uses the default implementation.
	 */
	return 0;
}

/*
 * Update inode size after write
 */
static void cfs_netfs_update_i_size(struct inode *inode, loff_t i_size)
{
	struct cfs_inode_info *ci = CFS_I(inode);
	
	cfs_debug("netfs_update_i_size: ino=%llu, size=%llu\n",
		  ci->i_ino, i_size);
	
	i_size_write(inode, i_size);
}

/*
 * Called when I/O is done
 */
static void cfs_netfs_done(struct netfs_io_request *rreq)
{
	cfs_debug("netfs_done: ino=%llu, transferred=%zu\n",
		  CFS_I(rreq->inode)->i_ino, rreq->transferred);
}

/*
 * Free a netfs subrequest
 */
static void cfs_netfs_free_subrequest(struct netfs_io_subrequest *subreq)
{
	cfs_debug("netfs_free_subrequest: start=%llu, len=%zu\n",
		  subreq->start, subreq->len);
	/* Nothing special to clean up - pages are managed by the core */
}

/*
 * Create write requests - split large write into RADOS-compatible chunks
 */
static void cfs_netfs_create_write_requests(struct netfs_io_request *wreq,
					   loff_t start, size_t len)
{
	struct netfs_io_subrequest *subreq;
	loff_t pos = start;
	size_t remaining = len;
	struct inode *inode = wreq->inode;
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("netfs_create_write_requests: ino=%llu, start=%llu, len=%zu\n",
		  ci->i_ino, start, len);

	/* Split the write into 4MB chunks aligned with RADOS objects */
	while (remaining > 0) {
		size_t chunk_size;
		u32 part_num = cfs_part_num(pos);
		u64 part_offset = cfs_part_offset(pos);

		/* Calculate chunk size - align to block boundary */
		chunk_size = min(remaining, (size_t)(CFS_BLOCK_SIZE - part_offset));

		/* Create a subrequest for this chunk */
		subreq = netfs_create_write_request(wreq, NETFS_UPLOAD_TO_SERVER,
						     pos, chunk_size, NULL);
		if (IS_ERR(subreq))
			break;

		cfs_debug("create_write_request: pos=%llu, len=%zu, part=%u\n",
			  pos, chunk_size, part_num);

		pos += chunk_size;
		remaining -= chunk_size;
	}
}

/*
 * Invalidate cache - called after write completes
 */
static void cfs_netfs_invalidate_cache(struct netfs_io_request *wreq)
{
	struct inode *inode = wreq->inode;
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("netfs_invalidate_cache: ino=%llu\n", ci->i_ino);
	/*
	 * CFS uses RADOS as the authoritative source, not a local cache.
	 * No invalidation needed - data is always read from RADOS.
	 */
}

/*
 * CFS netfs request operations table
 * Reference: ceph_netfs_ops
 */
const struct netfs_request_ops cfs_netfs_ops = {
	.io_request_size	= sizeof(struct netfs_io_request),
	.io_subrequest_size	= sizeof(struct netfs_io_subrequest),
	.init_request		= cfs_netfs_init_request,
	.free_request		= cfs_netfs_free_request,
	.free_subrequest	= cfs_netfs_free_subrequest,
	.expand_readahead	= cfs_netfs_expand_readahead,
	.clamp_length		= cfs_netfs_clamp_length,
	.issue_read		= cfs_netfs_issue_read,
	.issue_write		= cfs_netfs_issue_write,
	.is_still_valid		= cfs_netfs_is_still_valid,
	.check_write_begin	= cfs_netfs_check_write_begin,
	.done			= cfs_netfs_done,
	.update_i_size		= cfs_netfs_update_i_size,
	.create_write_requests	= cfs_netfs_create_write_requests,
	.invalidate_cache	= cfs_netfs_invalidate_cache,
};
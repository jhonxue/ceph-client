/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CFS - Ceph File System Simple
 *
 * Address space operations implementation
 * Uses netfs framework for all I/O operations (following ceph_aops pattern)
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/writeback.h>
#include <linux/uio.h>
#include <linux/ceph/osd_client.h>
#include <linux/netfs.h>

#include "super.h"
#include "internal.h"
#include "rados.h"

/*
 * CFS follows the same pattern as ceph_aops (fs/ceph/addr.c:1563-1574):
 *
 * - read operations: directly use netfs_read_folio, netfs_readahead
 * - write operations: custom implementation via netfs_request_ops callbacks
 * - release_folio: directly use netfs_release_folio
 * - direct_IO: use noop_direct_IO (netfs handles DIO internally)
 *
 * The actual I/O operations are implemented in netfs.c via netfs_request_ops.
 */

/*
 * Read data from file spanning multiple 4MB chunks
 * Used by netfs_request_ops.issue_read callback
 */
int cfs_read_data(struct cfs_fs_info *fsi, u64 ino, u64 offset,
		  u64 length, struct page **pages, int num_pages)
{
	u64 remaining = length;
	u64 current_offset = offset;
	int page_idx = 0;
	int ret;

	cfs_debug("read_data: ino=%llu, offset=%llu, length=%llu, pages=%d\n",
		  ino, offset, length, num_pages);

	while (remaining > 0 && page_idx < num_pages) {
		u32 part_num = cfs_part_num(current_offset);
		u64 part_offset = cfs_part_offset(current_offset);
		u64 chunk_len = min(remaining, CFS_BLOCK_SIZE - part_offset);
		struct ceph_object_id oid;
		int pages_in_chunk;

		cfs_data_oid(&oid, ino, part_num);

		/* Calculate pages needed for this chunk */
		pages_in_chunk = DIV_ROUND_UP(chunk_len, PAGE_SIZE);
		pages_in_chunk = min(pages_in_chunk, num_pages - page_idx);

		/* Read chunk */
		ret = cfs_data_read(fsi, &oid, part_offset, chunk_len,
				    &pages[page_idx], pages_in_chunk);
		if (ret) {
			cfs_err("read_data: failed at part %u: %d\n", part_num, ret);
			return ret;
		}

		/* Advance */
		current_offset += chunk_len;
		remaining -= chunk_len;
		page_idx += pages_in_chunk;
	}

	return 0;
}

/*
 * Write data to file spanning multiple 4MB chunks
 * Used by netfs_request_ops callbacks
 */
int cfs_write_data(struct cfs_fs_info *fsi, u64 ino, u64 offset,
		   u64 length, struct page **pages, int num_pages,
		   struct timespec64 *mtime)
{
	u64 remaining = length;
	u64 current_offset = offset;
	int page_idx = 0;
	int ret;

	cfs_debug("write_data: ino=%llu, offset=%llu, length=%llu, pages=%d\n",
		  ino, offset, length, num_pages);

	while (remaining > 0 && page_idx < num_pages) {
		u32 part_num = cfs_part_num(current_offset);
		u64 part_offset = cfs_part_offset(current_offset);
		u64 chunk_len = min(remaining, CFS_BLOCK_SIZE - part_offset);
		struct ceph_object_id oid;
		int pages_in_chunk;

		cfs_data_oid(&oid, ino, part_num);

		/* Calculate pages needed for this chunk */
		pages_in_chunk = DIV_ROUND_UP(chunk_len, PAGE_SIZE);
		pages_in_chunk = min(pages_in_chunk, num_pages - page_idx);

		/* Write chunk */
		ret = cfs_data_write(fsi, &oid, part_offset, chunk_len,
				     &pages[page_idx], pages_in_chunk);
		if (ret) {
			cfs_err("write_data: failed at part %u: %d\n", part_num, ret);
			return ret;
		}

		/* Advance */
		current_offset += chunk_len;
		remaining -= chunk_len;
		page_idx += pages_in_chunk;
	}

	/* Update metadata timestamps */
	if (mtime) {
		struct timespec64 now = current_time(fsi->sb);
		mtime->tv_sec = now.tv_sec;
		mtime->tv_nsec = now.tv_nsec;
	}

	return 0;
}

/*
 * Truncate file data
 */
int cfs_truncate_data(struct cfs_fs_info *fsi, u64 ino,
		      u64 old_size, u64 new_size, struct timespec64 *mtime)
{
	u32 old_max_part = cfs_max_part(old_size);
	u32 new_max_part = cfs_max_part(new_size);
	u32 part_num;
	struct ceph_object_id oid;
	int ret;

	cfs_debug("truncate_data: ino=%llu, old=%llu, new=%llu\n",
		  ino, old_size, new_size);

	if (new_size == 0) {
		/* Delete all data objects */
		for (part_num = 0; part_num < old_max_part; part_num++) {
			cfs_data_oid(&oid, ino, part_num);
			ret = cfs_data_delete(fsi, &oid);
			if (ret && ret != -ENOENT)
				cfs_debug("truncate_data: delete part %u failed: %d\n",
					  part_num, ret);
		}
		return 0;
	}

	/* Truncate or delete excess objects */
	for (part_num = new_max_part; part_num < old_max_part; part_num++) {
		cfs_data_oid(&oid, ino, part_num);
		ret = cfs_data_delete(fsi, &oid);
		if (ret && ret != -ENOENT)
			cfs_debug("truncate_data: delete part %u failed: %d\n",
				  part_num, ret);
	}

	/* Truncate last object if needed */
	if (new_max_part > 0 && (new_size & (CFS_BLOCK_SIZE - 1)) != 0) {
		cfs_data_oid(&oid, ino, new_max_part - 1);
		ret = cfs_data_truncate(fsi, &oid, new_size - (new_max_part - 1) * CFS_BLOCK_SIZE);
		if (ret)
			cfs_debug("truncate_data: truncate last part failed: %d\n", ret);
	}

	return 0;
}

/*
 * Delete all data objects for an inode
 */
int cfs_delete_data_objects(struct cfs_fs_info *fsi, u64 ino, u32 max_part)
{
	u32 part_num;
	struct ceph_object_id oid;
	int ret;

	cfs_debug("delete_data_objects: ino=%llu, max_part=%u\n", ino, max_part);

	for (part_num = 0; part_num < max_part; part_num++) {
		cfs_data_oid(&oid, ino, part_num);
		ret = cfs_data_delete(fsi, &oid);
		if (ret && ret != -ENOENT)
			cfs_debug("delete_data_objects: delete part %u failed: %d\n",
				  part_num, ret);
	}

	return 0;
}

/*
 * Write a single page to RADOS - follows ceph_writepage pattern
 * Netfs framework handles writeback via create_write_requests callback
 */
static int cfs_writepage(struct page *page, struct writeback_control *wbc)
{
	struct folio *folio = page_folio(page);
	struct inode *inode = folio->mapping->host;
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("writepage: ino=%llu, offset=%llu\n",
		  ci->i_ino, folio_pos(folio));

	/* Netfs framework handles writeback via create_write_requests callback */
	return netfs_writepage(page, wbc);
}

/*
 * Write multiple pages - follows ceph_writepages_start pattern
 * All writes go through netfs framework
 */
static int cfs_writepages(struct address_space *mapping,
			  struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("writepages: ino=%llu\n", ci->i_ino);

	/* All writes go through netfs framework */
	return netfs_writepages(mapping, wbc);
}

/*
 * Write begin - follows ceph_write_begin pattern (fs/ceph/addr.c:1503-1520)
 * Wraps netfs_write_begin, converting folio to page
 */
static int cfs_write_begin(struct file *file, struct address_space *mapping,
			   loff_t pos, unsigned len,
			   struct page **pagep, void **fsdata)
{
	struct inode *inode = file_inode(file);
	struct cfs_inode_info *ci = CFS_I(inode);
	struct folio *folio = NULL;
	int ret;

	cfs_debug("write_begin: ino=%llu, pos=%lld, len=%u\n",
		  ci->i_ino, pos, len);

	ret = netfs_write_begin(&ci->netfs, file, inode->i_mapping,
				pos, len, &folio, fsdata);
	if (ret < 0)
		return ret;

	WARN_ON_ONCE(!folio_test_locked(folio));
	*pagep = &folio->page;
	return 0;
}

/*
 * Write end - follows ceph_write_end pattern (fs/ceph/addr.c:1526-1561)
 * Netfs framework handles most of the work
 */
static int cfs_write_end(struct file *file, struct address_space *mapping,
			  loff_t pos, unsigned len, unsigned copied,
			  struct page *page, void *fsdata)
{
	struct folio *folio = page_folio(page);
	struct inode *inode = file_inode(file);
	struct cfs_inode_info *ci = CFS_I(inode);
	loff_t last_pos = pos + copied;

	cfs_debug("write_end: ino=%llu, pos=%lld, len=%u, copied=%u\n",
		  ci->i_ino, pos, len, copied);

	if (!folio_test_uptodate(folio)) {
		/* just return that nothing was copied on a short copy */
		if (copied < len) {
			copied = 0;
			goto out;
		}
		folio_mark_uptodate(folio);
	}

	/* did file size increase? */
	if (last_pos > i_size_read(inode))
		i_size_write(inode, last_pos);

	folio_mark_dirty(folio);
out:
	folio_unlock(folio);
	folio_put(folio);

	return copied;
}

/*
 * Invalidate folio - follows ceph_invalidate_folio pattern (fs/ceph/addr.c:137-163)
 * Just call netfs_invalidate_folio directly
 */
static void cfs_invalidate_folio(struct folio *folio, size_t offset,
				 size_t length)
{
	struct inode *inode = folio->mapping->host;

	cfs_debug("invalidate_folio: ino=%llu, offset=%zu, length=%zu\n",
		  CFS_I(inode)->i_ino, offset, length);

	/* Direct call to netfs, matching ceph behavior */
	netfs_invalidate_folio(folio, offset, length);
}

/*
 * Dirty folio - follows ceph_dirty_folio pattern (fs/ceph/addr.c:80-130)
 * Use netfs_dirty_folio for proper netfs integration
 * CFS doesn't need snap context tracking like Ceph, so use simplified version
 */
static bool cfs_dirty_folio(struct address_space *mapping, struct folio *folio)
{
	struct inode *inode = mapping->host;
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("dirty_folio: ino=%llu, index=%lu\n", ci->i_ino, folio->index);

	/* Use netfs_dirty_folio like ceph does via ceph_fscache_dirty_folio */
	return netfs_dirty_folio(mapping, folio);
}

/*
 * Address space operations - optimized to match ceph_aops pattern
 *
 * Reference: fs/ceph/addr.c:1563-1574
 *
 * Comparison with ceph_aops:
 * ┌─────────────────────┬────────────────────┬─────────────────────┐
 * │ Operation           │ ceph_aops          │ cfs_aops            │
 * ├─────────────────────┼────────────────────┼─────────────────────┤
 * │ read_folio          │ netfs_read_folio   │ netfs_read_folio    │
 * │ readahead           │ netfs_readahead    │ netfs_readahead     │
 * │ writepage           │ ceph_writepage     │ cfs_writepage       │
 * │ writepages          │ ceph_writepages    │ cfs_writepages      │
 * │ write_begin         │ ceph_write_begin   │ cfs_write_begin     │
 * │ write_end           │ ceph_write_end     │ cfs_write_end       │
 * │ dirty_folio         │ ceph_dirty_folio   │ cfs_dirty_folio     │
 * │ invalidate_folio    │ ceph_invalidate    │ cfs_invalidate      │
 * │ release_folio       │ netfs_release      │ netfs_release_folio │
 * │ direct_IO           │ noop_direct_IO     │ noop_direct_IO      │
 * └─────────────────────┴────────────────────┴─────────────────────┘
 */
const struct address_space_operations cfs_aops = {
	.read_folio		= netfs_read_folio,
	.readahead		= netfs_readahead,
	.writepage		= cfs_writepage,
	.writepages		= cfs_writepages,
	.write_begin		= cfs_write_begin,
	.write_end		= cfs_write_end,
	.dirty_folio		= cfs_dirty_folio,
	.invalidate_folio	= cfs_invalidate_folio,
	.release_folio		= netfs_release_folio,
	.direct_IO		= noop_direct_IO,
};
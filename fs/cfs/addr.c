/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CFS - Ceph File System Simple
 *
 * Address space operations implementation
 * Handles file data read/write with 4MB slicing
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/ceph/osd_client.h>

#include "super.h"
#include "internal.h"
#include "rados.h"

/*
 * Read a single folio from RADOS
 */
int cfs_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->file_mapping->host;
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_fs_info *fsi = CFS_SB(inode->i_sb);
	struct page *page = folio_page(folio, 0);
	struct page **pages;
	loff_t pos = folio_pos(folio);
	size_t len = folio_size(folio);
	u64 file_size = i_size_read(inode);
	u64 offset = pos;
	u64 read_len;
	u32 part_num;
	struct ceph_object_id oid;
	int ret;

	cfs_debug("read_folio: ino=%llu, pos=%lld, len=%zu, file_size=%llu\n",
		  ci->i_ino, pos, len, file_size);

	/* Handle reads beyond EOF */
	if (pos >= file_size) {
		folio_zero_segment(folio, 0, len);
		folio_mark_uptodate(folio);
		folio_unlock(folio);
		return 0;
	}

	/* Calculate read length */
	read_len = min(len, file_size - pos);
	if (read_len < len) {
		/* Zero portion beyond EOF */
		folio_zero_segment(folio, read_len, len);
	}

	/* Calculate which data object/part to read */
	part_num = cfs_part_num(offset);

	/* Read from RADOS */
	cfs_data_oid(&oid, ci->i_ino, part_num);

	/* Setup page pointer for read */
	pages = &page;

	ret = cfs_data_read(fsi, &oid, cfs_part_offset(offset),
			    read_len, pages, 1);
	if (ret) {
		cfs_err("read_folio: failed to read: %d\n", ret);
		folio_zero_segment(folio, 0, len);
		folio_mark_uptodate(folio);
		folio_unlock(folio);
		return ret;
	}

	folio_mark_uptodate(folio);
	folio_unlock(folio);
	return 0;
}

/*
 * Read multiple pages from RADOS
 */
static void cfs_readahead(struct readahead_control *rac)
{
	struct inode *inode = rac->file_mapping->host;
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_fs_info *fsi = CFS_SB(inode->i_sb);
	loff_t file_size = i_size_read(inode);
	struct page *page;
	loff_t start = readahead_pos(rac);
	size_t count = readahead_count(rac);
	u64 offset, read_len, remaining;
	u32 part_num, start_part;
	struct ceph_object_id oid;
	int ret;

	cfs_debug("readahead: ino=%llu, start=%lld, count=%zu\n",
		  ci->i_ino, start, count);

	/* Handle reads beyond EOF */
	if (start >= file_size)
		return;

	/* Calculate read parameters */
	start_part = cfs_part_num(start);
	remaining = min(count * PAGE_SIZE, file_size - start);

	while (remaining > 0 && readahead_count(rac) > 0) {
		page = readahead_page(rac);
		if (!page)
			break;

		offset = page_offset(page);
		part_num = cfs_part_num(offset);

		read_len = min((u64)PAGE_SIZE, remaining);
		read_len = min(read_len, cfs_part_remaining(offset, read_len));

		/* Read from RADOS */
		cfs_data_oid(&oid, ci->i_ino, part_num);
		ret = cfs_data_read(fsi, &oid, cfs_part_offset(offset),
				    read_len, &page, 1);

		if (ret) {
			SetPageError(page);
			zero_user_segment(page, 0, PAGE_SIZE);
		} else {
			if (read_len < PAGE_SIZE)
				zero_user_segment(page, read_len, PAGE_SIZE);
			SetPageUptodate(page);
		}

		unlock_page(page);
		put_page(page);

		remaining -= read_len;
	}
}

/*
 * Write begin - prepare page for write
 */
int cfs_write_begin(struct file *file, struct address_space *mapping,
		    loff_t pos, unsigned len, struct folio **foliop,
		    void **fsdata)
{
	struct inode *inode = mapping->host;
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_fs_info *fsi = CFS_SB(inode->i_sb);
	struct folio *folio;
	int ret;

	cfs_debug("write_begin: ino=%llu, pos=%lld, len=%u\n",
		  ci->i_ino, pos, len);

	/* Get or create folio */
	folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT,
				    FGP_WRITEBEGIN | FGP_CREAT | FGP_LOCK,
				    mapping->gfp_mask);
	if (!folio)
		return -ENOMEM;

	/* If not uptodate, read from RADOS */
	if (!folio_test_uptodate(folio)) {
		u64 offset = folio_pos(folio);
		u32 part_num = cfs_part_num(offset);
		struct ceph_object_id oid;
		struct page *page = folio_page(folio, 0);
		u64 file_size = i_size_read(inode);

		/* If writing to new area beyond EOF, just zero */
		if (offset >= file_size) {
			folio_zero_segment(folio, 0, folio_size(folio));
		} else {
			/* Read existing data */
			size_t read_len = min(folio_size(folio),
					      file_size - offset);

			cfs_data_oid(&oid, ci->i_ino, part_num);
			ret = cfs_data_read(fsi, &oid, cfs_part_offset(offset),
					    read_len, &page, 1);
			if (ret && ret != -ENOENT) {
				folio_unlock(folio);
				folio_put(folio);
				return ret;
			}

			/* Zero unread portion */
			if (read_len < folio_size(folio))
				folio_zero_segment(folio, read_len,
						   folio_size(folio));
		}

		folio_mark_uptodate(folio);
	}

	*foliop = folio;
	*fsdata = NULL;
	return 0;
}

/*
 * Write end - commit write to RADOS
 */
int cfs_write_end(struct file *file, struct address_space *mapping,
		  loff_t pos, unsigned len, unsigned copied,
		  struct folio *folio, void *fsdata)
{
	struct inode *inode = mapping->host;
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_fs_info *fsi = CFS_SB(inode->i_sb);
	struct timespec64 now;
	u64 offset = pos;
	u64 write_len = copied;
	u32 part_num;
	struct ceph_object_id oid;
	struct page *page = folio_page(folio, 0);
	int ret;

	cfs_debug("write_end: ino=%llu, pos=%lld, len=%u, copied=%u\n",
		  ci->i_ino, pos, len, copied);

	/* Update file size if needed */
	if (pos + copied > i_size_read(inode)) {
		i_size_write(inode, pos + copied);
		ci->i_blocks = cfs_max_part(i_size_read(inode));
	}

	/* Calculate which data object/part to write */
	part_num = cfs_part_num(offset);

	/* Write to RADOS */
	cfs_data_oid(&oid, ci->i_ino, part_num);
	ret = cfs_data_write(fsi, &oid, cfs_part_offset(offset),
			     write_len, &page, 1);
	if (ret) {
		cfs_err("write_end: failed to write: %d\n", ret);
		folio_unlock(folio);
		folio_put(folio);
		return ret;
	}

	/* Update inode metadata */
	now = current_time(inode);
	spin_lock(&inode->i_lock);
	inode->i_mtime = now;
	inode_set_ctime(inode, now.tv_sec, now.tv_nsec);
	spin_unlock(&inode->i_lock);

	/* Mark page dirty */
	folio_mark_uptodate(folio);
	folio_unlock(folio);
	folio_put(folio);

	return copied;
}

/*
 * Read data from file spanning multiple 4MB chunks
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
		int i;

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
		int i;

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
		mtime->tv_sec = current_time(fsi->sb).tv_sec;
		mtime->tv_nsec = current_time(fsi->sb).tv_nsec;
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
 * Address space operations
 */
const struct address_space_operations cfs_aops = {
	.read_folio     = cfs_read_folio,
	.readahead      = cfs_readahead,
	.write_begin    = cfs_write_begin,
	.write_end      = cfs_write_end,
	.dirty_folio    = filemap_dirty_folio,
	.migrate_folio  = filemap_migrate_folio,
	.invalidate_folio = filemap_invalidate_folio,
	.release_folio  = filemap_release_folio,
	.is_partially_uptodate = filemap_is_partially_uptodate,
};
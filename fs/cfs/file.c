/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CFS - Ceph File System Simple
 *
 * File operations implementation
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/uio.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/ceph/osd_client.h>
#include <linux/netfs.h>

#include "super.h"
#include "internal.h"
#include "rados.h"

/*
 * File open handler
 */
int cfs_file_open(struct inode *inode, struct file *file)
{
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("file_open: ino=%llu\n", ci->i_ino);

	return generic_file_open(inode, file);
}

/*
 * File release handler
 */
int cfs_file_release(struct inode *inode, struct file *file)
{
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("file_release: ino=%llu\n", ci->i_ino);

	return 0;
}

/*
 * Read iteration - uses netfs framework
 */
ssize_t cfs_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("file_read_iter: ino=%llu, pos=%lld, count=%zu\n",
		  ci->i_ino, iocb->ki_pos, iov_iter_count(iter));

	return netfs_file_read_iter(iocb, iter);
}

/*
 * Write iteration - uses netfs framework
 */
ssize_t cfs_file_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("file_write_iter: ino=%llu, pos=%lld, count=%zu\n",
		  ci->i_ino, iocb->ki_pos, iov_iter_count(iter));

	/*
	 * netfs_file_write_iter handles its own inode locking
	 * to avoid potential deadlocks with concurrent reads.
	 */
	return netfs_file_write_iter(iocb, iter);
}

/*
 * Map file data to pages
 */
static int cfs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(file);
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("file_mmap: ino=%llu\n", ci->i_ino);

	return generic_file_mmap(file, vma);
}

/*
 * Get file attributes
 */
int cfs_file_getattr(struct mnt_idmap *idmap, const struct path *path,
		     struct kstat *stat, u32 request_mask,
		     unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_fs_info *fsi = CFS_SB(inode->i_sb);
	struct cfs_dentry_metadata dmeta;
	u64 parent_ino;
	int ret;

	cfs_debug("file_getattr: ino=%llu\n", ci->i_ino);

	/* Read fresh metadata from RADOS for parent */
	parent_ino = ci->i_parent_ino;
	if (parent_ino > 0) {
		/* Find name - this is complex, use cached version for now */
	}

	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

/*
 * Set file attributes
 */
int cfs_file_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		     struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_fs_info *fsi = CFS_SB(inode->i_sb);
	struct cfs_dentry_metadata dmeta;
	unsigned int ia_valid = attr->ia_valid;
	int ret;

	cfs_debug("file_setattr: ino=%llu, valid=%x\n", ci->i_ino, ia_valid);

	/* Apply standard setattr */
	ret = simple_setattr(idmap, dentry, attr);
	if (ret)
		return ret;

	/* Sync metadata to RADOS if needed */
	if (ia_valid & (ATTR_MODE | ATTR_UID | ATTR_GID | ATTR_SIZE |
			ATTR_ATIME | ATTR_MTIME | ATTR_CTIME)) {
		/* Update parent's omap entry */
		/* Note: need parent and name to update - complex */
	}

	return 0;
}

/*
 * File operations for regular files
 */
const struct file_operations cfs_file_ops = {
	.open           = cfs_file_open,
	.release        = cfs_file_release,
	.read_iter      = cfs_file_read_iter,
	.write_iter     = cfs_file_write_iter,
	.mmap           = cfs_file_mmap,
	.llseek         = generic_file_llseek,
	.splice_read    = filemap_splice_read,
	.fsync          = generic_file_fsync,
};

/*
 * Splice data to pipe
 */
static ssize_t cfs_file_splice_read(struct file *in, loff_t *ppos,
				    struct pipe_inode_info *pipe,
				    size_t len, unsigned int flags)
{
	return filemap_splice_read(in, ppos, pipe, len, flags);
}
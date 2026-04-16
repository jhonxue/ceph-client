/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FS_CFS_INTERNAL_H
#define _FS_CFS_INTERNAL_H

#include "super.h"

/*
 * CFS Internal definitions
 */

/* Debug macros */
#define CFS_DEBUG 1

#ifdef CFS_DEBUG
#define cfs_debug(fmt, ...) pr_debug("cfs: " fmt, ##__VA_ARGS__)
#else
#define cfs_debug(fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#endif

#define cfs_info(fmt, ...)  pr_info("cfs: " fmt, ##__VA_ARGS__)
#define cfs_warn(fmt, ...)  pr_warn("cfs: " fmt, ##__VA_ARGS__)
#define cfs_err(fmt, ...)   pr_err("cfs: " fmt, ##__VA_ARGS__)

/* Mount states */
#define CFS_MOUNT_UNMOUNTED   0
#define CFS_MOUNT_MOUNTING    1
#define CFS_MOUNT_MOUNTED     2
#define CFS_MOUNT_UNMOUNTING  3

/* Error handling */
#define cfs_check_ret(ret, action) \
	do { \
		if (ret < 0) { \
			cfs_err(action" failed: %d\n", ret); \
			goto out; \
		} \
	} while (0)

/* Directory entry metadata structure version */
#define CFS_DENTRY_METADATA_V1     1
#define CFS_DENTRY_METADATA_COMPAT  1

/* Allocator data version */
#define CFS_ALLOCATOR_DATA_V1       1
#define CFS_ALLOCATOR_DATA_COMPAT   1

/* OSD operation timeouts */
#define CFS_OSD_REQUEST_TIMEOUT    msecs_to_jiffies(60 * 1000)  /* 60 seconds */
#define CFS_OSD_REQUEST_RETRIES    3

/* Memory helpers */
static inline void *cfs_kzalloc(size_t size, gfp_t flags)
{
	return kzalloc(size, flags);
}

static inline void cfs_kvfree(void *ptr)
{
	kvfree(ptr);
}

/* Time helpers */
static inline void cfs_current_time(struct timespec64 *ts)
{
	ktime_get_real_ts64(ts);
}

static inline void cfs_set_inode_time(struct inode *inode, struct timespec64 *ts)
{
	*ts = inode_set_ctime_current(inode);
	inode->i_mtime = *ts;
	inode->i_atime = *ts;
}

/* Mode helpers */
static inline bool cfs_is_dir(umode_t mode)
{
	return S_ISDIR(mode);
}

static inline bool cfs_is_file(umode_t mode)
{
	return S_ISREG(mode);
}

static inline bool cfs_is_symlink(umode_t mode)
{
	return S_ISLNK(mode);
}

/* Structure length calculation */
static inline size_t cfs_dentry_metadata_len(unsigned int name_len,
					     unsigned int symlink_len)
{
	size_t len = sizeof(struct cfs_dentry_metadata);
	len += name_len;
	if (symlink_len > 0)
		len += symlink_len;
	return len;
}

/* Initialize directory entry metadata */
static inline void cfs_init_dentry_metadata(struct cfs_dentry_metadata *dmeta,
					     u64 ino, const char *name,
					     unsigned int name_len,
					     umode_t mode, u64 parent_ino)
{
	struct timespec64 now;

	dmeta->struct_v = CFS_DENTRY_METADATA_V1;
	dmeta->struct_compat = CFS_DENTRY_METADATA_COMPAT;
	dmeta->struct_len = cfs_dentry_metadata_len(name_len, 0);
	dmeta->ino = cpu_to_le64(ino);
	dmeta->name_len = cpu_to_le32(name_len);
	memcpy(dmeta + 1, name, name_len);

	dmeta->mode = cpu_to_le32(mode);
	dmeta->size = 0;
	dmeta->blocks = 0;

	cfs_current_time(&now);
	dmeta->atime.tv_sec = cpu_to_le32(now.tv_sec);
	dmeta->atime.tv_nsec = cpu_to_le32(now.tv_nsec);
	dmeta->mtime.tv_sec = cpu_to_le32(now.tv_sec);
	dmeta->mtime.tv_nsec = cpu_to_le32(now.tv_nsec);
	dmeta->ctime.tv_sec = cpu_to_le32(now.tv_sec);
	dmeta->ctime.tv_nsec = cpu_to_le32(now.tv_nsec);

	dmeta->nlink = cpu_to_le32(S_ISDIR(mode) ? 2 : 1);
	dmeta->uid = cpu_to_le32(from_kuid(&init_user_ns, current_fsuid()));
	dmeta->gid = cpu_to_le32(from_kgid(&init_user_ns, current_fsgid()));

	dmeta->parent_ino = cpu_to_le64(parent_ino);
	dmeta->rdev = 0;
	dmeta->symlink_len = 0;
}

/* Copy from dentry metadata to inode */
static inline void cfs_dentry_to_inode(struct cfs_dentry_metadata *dmeta,
					struct inode *inode)
{
	struct cfs_inode_info *ci = CFS_I(inode);

	ci->i_ino = le64_to_cpu(dmeta->ino);
	ci->i_parent_ino = le64_to_cpu(dmeta->parent_ino);
	inode->i_mode = le32_to_cpu(dmeta->mode);
	i_size_write(inode, le64_to_cpu(dmeta->size));
	ci->i_blocks = le64_to_cpu(dmeta->blocks);
	set_nlink(inode, le32_to_cpu(dmeta->nlink));
	inode->i_uid = make_kuid(&init_user_ns, le32_to_cpu(dmeta->uid));
	inode->i_gid = make_kgid(&init_user_ns, le32_to_cpu(dmeta->gid));
	inode->i_atime.tv_sec = le32_to_cpu(dmeta->atime.tv_sec);
	inode->i_atime.tv_nsec = le32_to_cpu(dmeta->atime.tv_nsec);
	inode->i_mtime.tv_sec = le32_to_cpu(dmeta->mtime.tv_sec);
	inode->i_mtime.tv_nsec = le32_to_cpu(dmeta->mtime.tv_nsec);
	inode_set_ctime(inode,
			(s64)le32_to_cpu(dmeta->ctime.tv_sec),
			(s64)le32_to_cpu(dmeta->ctime.tv_nsec));
	inode->i_rdev = le64_to_cpu(dmeta->rdev);
}

/* Copy from inode to dentry metadata */
static inline void cfs_inode_to_dentry(struct inode *inode,
					struct cfs_dentry_metadata *dmeta)
{
	struct cfs_inode_info *ci = CFS_I(inode);
	struct timespec64 ts;

	dmeta->struct_v = CFS_DENTRY_METADATA_V1;
	dmeta->struct_compat = CFS_DENTRY_METADATA_COMPAT;

	dmeta->ino = cpu_to_le64(ci->i_ino);
	dmeta->parent_ino = cpu_to_le64(ci->i_parent_ino);
	dmeta->mode = cpu_to_le32(inode->i_mode);
	dmeta->size = cpu_to_le64(i_size_read(inode));
	dmeta->blocks = cpu_to_le64(ci->i_blocks);
	dmeta->nlink = cpu_to_le32(inode->i_nlink);
	dmeta->uid = cpu_to_le32(from_kuid(&init_user_ns, inode->i_uid));
	dmeta->gid = cpu_to_le32(from_kgid(&init_user_ns, inode->i_gid));

	ts = inode_get_ctime(inode);
	dmeta->ctime.tv_sec = cpu_to_le32(ts.tv_sec);
	dmeta->ctime.tv_nsec = cpu_to_le32(ts.tv_nsec);

	dmeta->atime.tv_sec = cpu_to_le32(inode->i_atime.tv_sec);
	dmeta->atime.tv_nsec = cpu_to_le32(inode->i_atime.tv_nsec);
	dmeta->mtime.tv_sec = cpu_to_le32(inode->i_mtime.tv_sec);
	dmeta->mtime.tv_nsec = cpu_to_le32(inode->i_mtime.tv_nsec);

	dmeta->rdev = cpu_to_le64(inode->i_rdev);
	dmeta->symlink_len = 0;
}

#endif /* _FS_CFS_INTERNAL_H */
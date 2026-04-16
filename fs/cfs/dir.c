/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CFS - Ceph File System Simple
 *
 * Directory operations implementation
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/dcache.h>
#include <linux/ceph/osd_client.h>

#include "super.h"
#include "internal.h"
#include "rados.h"

/*
 * Create a directory
 */
int cfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
	      struct dentry *dentry, umode_t mode)
{
	struct cfs_fs_info *fsi = CFS_SB(dir->i_sb);
	struct cfs_inode_info *dir_ci = CFS_I(dir);
	struct cfs_dentry_metadata *dmeta;
	struct ceph_object_id oid;
	int new_ino;
	unsigned int name_len = dentry->d_name.len;
	size_t dmeta_size;
	struct inode *inode;
	struct timespec64 now;
	int ret;

	cfs_debug("cfs_mkdir: dir_ino=%llu, name=%.*s, mode=%o\n",
		  dir_ci->i_ino, name_len, dentry->d_name.name, mode);

	/* Allocate inode */
	new_ino = cfs_alloc_ino(fsi);
	if (new_ino < 0)
		return new_ino;

	/* Allocate metadata buffer */
	dmeta_size = cfs_dentry_metadata_len(name_len, 0);
	dmeta = kzalloc(dmeta_size, GFP_KERNEL);
	if (!dmeta)
		return -ENOMEM;

	/* Initialize metadata */
	dmeta->struct_v = CFS_DENTRY_METADATA_V1;
	dmeta->struct_compat = CFS_DENTRY_METADATA_COMPAT;
	dmeta->ino = cpu_to_le64(new_ino);
	dmeta->parent_ino = cpu_to_le64(dir_ci->i_ino);
	dmeta->mode = cpu_to_le32(S_IFDIR | mode);
	dmeta->size = 0;
	dmeta->blocks = 0;
	dmeta->nlink = cpu_to_le32(2);  /* . and .. */
	dmeta->uid = cpu_to_le32(from_kuid(&init_user_ns, current_fsuid()));
	dmeta->gid = cpu_to_le32(from_kgid(&init_user_ns, current_fsgid()));
	dmeta->name_len = cpu_to_le32(name_len);
	dmeta->symlink_len = 0;

	now = current_time(dir);
	dmeta->mtime.tv_sec = cpu_to_le32(now.tv_sec);
	dmeta->mtime.tv_nsec = cpu_to_le32(now.tv_nsec);
	dmeta->atime = dmeta->mtime;
	dmeta->ctime = dmeta->mtime;

	/* Copy name */
	memcpy((void *)dmeta + sizeof(*dmeta), dentry->d_name.name, name_len);

	/* Create directory entry in parent */
	ret = cfs_create_dentry(fsi, dir_ci->i_ino,
			     dentry->d_name.name, name_len, dmeta, true);
	if (ret) {
		kfree(dmeta);
		return ret;
	}

	/* Create meta object for new directory */
	cfs_meta_oid(&oid, new_ino);

	/* Create empty meta object (for future entries) */
	ret = cfs_omap_write(fsi, &oid, ".empty", "", 0);
	if (ret) {
		cfs_remove_dentry(fsi, dir_ci->i_ino, dentry->d_name.name, name_len);
		kfree(dmeta);
		return ret;
	}

	/* Create inode */
	inode = cfs_iget(dir->i_sb, new_ino, dmeta);
	kfree(dmeta);

	if (IS_ERR(inode)) {
		cfs_remove_dentry(fsi, dir_ci->i_ino, dentry->d_name.name, name_len);
		return PTR_ERR(inode);
	}

	/* Update parent directory */
	spin_lock(&dir->i_lock);
	inc_nlink(dir);
	dir->i_mtime = now;
	inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
	spin_unlock(&dir->i_lock);

	d_instantiate(dentry, inode);
	return 0;
}

/*
 * Remove a directory
 */
int cfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct cfs_fs_info *fsi = CFS_SB(dir->i_sb);
	struct cfs_inode_info *dir_ci = CFS_I(dir);
	struct inode *inode = d_inode(dentry);
	struct cfs_inode_info *ci = CFS_I(inode);
	struct ceph_object_id oid;
	struct timespec64 now;
	bool has_entries = false;
	int ret;

	cfs_debug("cfs_rmdir: dir_ino=%llu, name=%.*s, target_ino=%llu\n",
		  dir_ci->i_ino, dentry->d_name.len, dentry->d_name.name,
		  ci->i_ino);

	/* Check if directory is empty */
	cfs_meta_oid(&oid, ci->i_ino);
	ret = cfs_omap_list(fsi, &oid, NULL, NULL, 1,
			cfs_dir_list_check_empty, &has_entries);
	if (ret && ret != -ENOENT)
		return ret;
	if (has_entries)
		return -ENOTEMPTY;

	/* Remove from parent directory */
	ret = cfs_remove_dentry(fsi, dir_ci->i_ino,
			     dentry->d_name.name, dentry->d_name.len);
	if (ret)
		return ret;

	/* Delete meta object for directory */
	cfs_meta_oid(&oid, ci->i_ino);
	ret = cfs_data_delete(fsi, &oid);
	if (ret)
		cfs_debug("cfs_rmdir: failed to delete meta object: %d\n", ret);

	/* Update inode */
	spin_lock(&inode->i_lock);
	drop_nlink(inode);
	drop_nlink(inode);  /* Two links for directory */
	clear_nlink(inode);
	now = current_time(inode);
	inode_set_ctime(inode, now.tv_sec, now.tv_nsec);
	spin_unlock(&inode->i_lock);

	/* Update parent */
	spin_lock(&dir->i_lock);
	drop_nlink(dir);
	dir->i_mtime = now;
	inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
	spin_unlock(&dir->i_lock);

	dput(dentry);
	return 0;
}

/*
 * Directory listing callback for checking emptiness
 */
int cfs_dir_list_check_empty(const char *key, void *value, size_t len, void *priv)
{
	bool *has_entries = priv;

	/* Skip special keys */
	if (strcmp(key, ".empty") == 0)
		return 0;

	*has_entries = true;
	return 1;  /* Stop iteration */
}

/*
 * Directory listing callback for readdir
 */
struct cfs_readdir_ctx {
	struct dir_context ctx;
	struct cfs_fs_info *fsi;
	u64 dir_ino;
	u64 last_hash;
};

static int cfs_readdir_cb(const char *key, void *value, size_t len, void *priv)
{
	struct cfs_readdir_ctx *rctx = priv;
	struct cfs_dentry_metadata *dmeta = value;
	const char *name;
	unsigned int name_len;
	u64 ino;
	umode_t mode;

	/* Skip special keys */
	if (strcmp(key, ".empty") == 0)
		return 0;

	/* Parse key: "hash_name" */
	name = strchr(key, '_');
	if (!name)
		return 0;
	name++;  /* Skip underscore */

	name_len = strlen(name);
	ino = le64_to_cpu(dmeta->ino);
	mode = le32_to_cpu(dmeta->mode);

	cfs_debug("readdir: name=%s, ino=%llu, mode=%o\n", name, ino, mode);

	/* Emit to VFS */
	if (!dir_emit(rctx->ctx, name, name_len, ino,
		      cfs_mode_to_type(mode)))
		return 1;  /* Stop iteration */

	return 0;
}

/*
 * Map mode to directory entry type
 */
static unsigned char cfs_mode_to_type(umode_t mode)
{
	if (S_ISDIR(mode))
		return DT_DIR;
	if (S_ISREG(mode))
		return DT_REG;
	if (S_ISLNK(mode))
		return DT_LNK;
	if (S_ISCHR(mode))
		return DT_CHR;
	if (S_ISBLK(mode))
		return DT_BLK;
	if (S_ISFIFO(mode))
		return DT_FIFO;
	if (S_ISSOCK(mode))
		return DT_SOCK;
	return DT_UNKNOWN;
}

/*
 * Read directory entries
 */
int cfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct cfs_fs_info *fsi = CFS_SB(inode->i_sb);
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_readdir_ctx rctx;
	struct ceph_object_id oid;
	int ret;

	cfs_debug("cfs_readdir: dir_ino=%llu, pos=%llu\n",
		  ci->i_ino, ctx->pos);

	if (!dir_emit_dots(file, ctx))
		return 0;

	/* Build object ID */
	cfs_meta_oid(&oid, ci->i_ino);

	/* Set up readdir context */
	rctx.ctx = *ctx;
	rctx.fsi = fsi;
	rctx.dir_ino = ci->i_ino;
	rctx.last_hash = ctx->pos;

	/* List omap entries */
	ret = cfs_omap_list(fsi, &oid, NULL, NULL, 0, cfs_readdir_cb, &rctx);
	if (ret && ret != -ENOENT) {
		cfs_err("cfs_readdir: failed: %d\n", ret);
		return ret;
	}

	ctx->pos = rctx.ctx.pos;
	return 0;
}

/*
 * Rename a file or directory
 */
int cfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
	       struct dentry *old_dentry, struct inode *new_dir,
	       struct dentry *new_dentry, unsigned int flags)
{
	struct cfs_fs_info *fsi = CFS_SB(old_dir->i_sb);
	struct cfs_inode_info *old_dir_ci = CFS_I(old_dir);
	struct cfs_inode_info *new_dir_ci = CFS_I(new_dir);
	struct inode *inode = d_inode(old_dentry);
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_dentry_metadata dmeta;
	struct timespec64 now;
	int ret;

	cfs_debug("cfs_rename: old_dir=%llu, old_name=%.*s, new_dir=%llu, new_name=%.*s\n",
		  old_dir_ci->i_ino, old_dentry->d_name.len, old_dentry->d_name.name,
		  new_dir_ci->i_ino, new_dentry->d_name.len, new_dentry->d_name.name);

	/* Prevent rename to self */
	if (old_dir == new_dir &&
	    strcmp(old_dentry->d_name.name, new_dentry->d_name.name) == 0)
		return 0;

	/* Check if target exists */
	struct inode *new_inode = d_inode(new_dentry);
	if (new_inode) {
		struct cfs_inode_info *new_ci = CFS_I(new_inode);

		/* Cannot overwrite directory with non-directory */
		if (S_ISDIR(inode->i_mode) && !S_ISDIR(new_inode->i_mode))
			return -ENOTDIR;
		/* Cannot overwrite non-directory with directory */
		if (!S_ISDIR(inode->i_mode) && S_ISDIR(new_inode->i_mode))
			return -ISDIR;

		/* Remove target */
		if (S_ISDIR(new_inode->i_mode)) {
			ret = cfs_rmdir(new_dir, new_dentry);
		} else {
			ret = cfs_unlink(new_dir, new_dentry);
		}
		if (ret)
			return ret;
	}

	/* Read old metadata */
	ret = cfs_lookup_dentry(fsi, old_dir_ci->i_ino,
			     old_dentry->d_name.name, old_dentry->d_name.len,
			     &dmeta);
	if (ret)
		return ret;

	/* Update parent ino if moving directory */
	if (S_ISDIR(inode->i_mode)) {
		dmeta.parent_ino = cpu_to_le64(new_dir_ci->i_ino);
	}

	/* Update name in metadata */
	dmeta.name_len = cpu_to_le32(new_dentry->d_name.len);
	memcpy((void *)&dmeta + offsetof(struct cfs_dentry_metadata, name_len) + 4,
	       new_dentry->d_name.name, new_dentry->d_name.len);

	/* Create new entry */
	ret = cfs_create_dentry(fsi, new_dir_ci->i_ino,
			     new_dentry->d_name.name, new_dentry->d_name.len,
			     &dmeta, false);
	if (ret)
		return ret;

	/* Remove old entry */
	ret = cfs_remove_dentry(fsi, old_dir_ci->i_ino,
			     old_dentry->d_name.name, old_dentry->d_name.len);
	if (ret) {
		cfs_remove_dentry(fsi, new_dir_ci->i_ino,
			       new_dentry->d_name.name, new_dentry->d_name.len);
		return ret;
	}

	/* Update timestamps */
	now = current_time(inode);

	spin_lock(&inode->i_lock);
	inode_set_ctime(inode, now.tv_sec, now.tv_nsec);
	spin_unlock(&inode->i_lock);

	spin_lock(&old_dir->i_lock);
	old_dir->i_mtime = now;
	inode_set_ctime(old_dir, now.tv_sec, now.tv_nsec);
	spin_unlock(&old_dir->i_lock);

	if (old_dir != new_dir) {
		spin_lock(&new_dir->i_lock);
		if (S_ISDIR(inode->i_mode)) {
			inc_nlink(new_dir);
		}
		new_dir->i_mtime = now;
		inode_set_ctime(new_dir, now.tv_sec, now.tv_nsec);
		spin_unlock(&new_dir->i_lock);

		spin_lock(&old_dir->i_lock);
		if (S_ISDIR(inode->i_mode)) {
			drop_nlink(old_dir);
		}
		spin_unlock(&old_dir->i_lock);
	}

	/* Update inode cache */
	if (!new_inode) {
		d_move(old_dentry, new_dentry);
	}

	return 0;
}

/*
 * Directory file operations
 */
const struct file_operations cfs_dir_file_ops = {
	.iterate_shared = cfs_readdir,
	.llseek         = generic_file_llseek,
	.read           = generic_read_dir,
};
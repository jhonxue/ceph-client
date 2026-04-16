/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CFS - Ceph File System Simple
 *
 * Inode operations implementation
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/ceph/ceph_client.h>

#include "super.h"
#include "internal.h"
#include "rados.h"

extern struct kmem_cache *cfs_inode_cachep;

/*
 * Compare inode number for ilookup5
 */
int cfs_ino_eq(struct inode *inode, void *data)
{
	u64 *ino = data;
	return CFS_I(inode)->i_ino == *ino;
}

/*
 * Lookup a directory entry in parent directory's omap
 */
int cfs_lookup_dentry(struct cfs_fs_info *fsi, u64 dir_ino,
		      const char *name, unsigned int name_len,
		      struct cfs_dentry_metadata *dmeta)
{
	struct ceph_object_id oid;
	char key[256];
	size_t value_len;
	int ret;

	cfs_debug("lookup_dentry: dir_ino=%llu, name=%.*s\n",
		  dir_ino, name_len, name);

	/* Build omap key: <hash>_<filename> */
	cfs_make_dentry_key(key, sizeof(key), name, name_len);

	/* Build object ID for parent directory */
	cfs_meta_oid(&oid, dir_ino);

	/* Read from omap */
	value_len = sizeof(*dmeta) + name_len + 256;  /* Extra space for symlink */
	ret = cfs_omap_read(fsi, &oid, key, dmeta, &value_len);
	if (ret) {
		cfs_debug("lookup_dentry: not found: %d\n", ret);
		return ret;
	}

	cfs_debug("lookup_dentry: found ino=%llu\n", le64_to_cpu(dmeta->ino));
	return 0;
}

/*
 * Create a directory entry atomically
 * Uses optimistic concurrency: check non-existence, write, verify
 */
int cfs_create_dentry(struct cfs_fs_info *fsi, u64 dir_ino,
		      const char *name, unsigned int name_len,
		      struct cfs_dentry_metadata *dmeta, bool exclusive)
{
	struct ceph_object_id oid;
	char key[256];
	size_t dmeta_len;
	bool exists = false;
	u64 written_ino, read_ino;
	int ret, retries = 0;

	cfs_debug("create_dentry: dir_ino=%llu, name=%.*s, ino=%llu\n",
		  dir_ino, name_len, name, le64_to_cpu(dmeta->ino));

	/* Build omap key */
	cfs_make_dentry_key(key, sizeof(key), name, name_len);

	/* Build object ID */
	cfs_meta_oid(&oid, dir_ino);

	/* Calculate metadata length */
	dmeta_len = cfs_dentry_metadata_len(name_len, le32_to_cpu(dmeta->symlink_len));
	memcpy((void *)dmeta + offsetof(struct cfs_dentry_metadata, name_len) + 4,
	       name, name_len);

retry:
	if (retries++ >= CFS_MAX_RETRIES) {
		cfs_err("create_dentry: max retries exceeded\n");
		return -EIO;
	}

	/* Step 1: Check if entry already exists */
	if (exclusive) {
		ret = cfs_omap_exists(fsi, &oid, key, &exists);
		if (ret)
			return ret;
		if (exists) {
			cfs_debug("create_dentry: entry already exists\n");
			return -EEXIST;
		}
	}

	/* Step 2: Write the entry */
	written_ino = le64_to_cpu(dmeta->ino);
	ret = cfs_omap_write(fsi, &oid, key, dmeta, dmeta_len);
	if (ret) {
		cfs_debug("create_dentry: write failed: %d\n", ret);
		return ret;
	}

	/* Step 3: Verify our inode was written (optimistic concurrency check) */
	if (exclusive) {
		struct cfs_dentry_metadata verify_dmeta;
		size_t verify_len = sizeof(verify_dmeta) + name_len;

		ret = cfs_omap_read(fsi, &oid, key, &verify_dmeta, &verify_len);
		if (ret == -ENOENT) {
			/* Entry disappeared, retry */
			cfs_debug("create_dentry: entry disappeared, retrying\n");
			goto retry;
		}
		if (ret)
			return ret;

		read_ino = le64_to_cpu(verify_dmeta.ino);
		if (read_ino != written_ino) {
			/* Another node created entry first, conflict */
			cfs_debug("create_dentry: conflict detected, read_ino=%llu, written_ino=%llu\n",
				  read_ino, written_ino);
			return -EEXIST;
		}
	}

	cfs_debug("create_dentry: created successfully\n");
	return 0;
}

/*
 * Remove a directory entry
 */
int cfs_remove_dentry(struct cfs_fs_info *fsi, u64 dir_ino,
		      const char *name, unsigned int name_len)
{
	struct ceph_object_id oid;
	char key[256];
	int ret;

	cfs_debug("remove_dentry: dir_ino=%llu, name=%.*s\n",
		  dir_ino, name_len, name);

	/* Build omap key */
	cfs_make_dentry_key(key, sizeof(key), name, name_len);

	/* Build object ID */
	cfs_meta_oid(&oid, dir_ino);

	/* Delete from omap */
	ret = cfs_omap_delete(fsi, &oid, key);
	if (ret) {
		cfs_debug("remove_dentry: failed: %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * Update directory entry metadata
 */
int cfs_update_dentry(struct cfs_fs_info *fsi, u64 dir_ino,
		      const char *name, unsigned int name_len,
		      struct cfs_dentry_metadata *dmeta)
{
	struct ceph_object_id oid;
	char key[256];
	size_t dmeta_len;
	int ret;

	cfs_debug("update_dentry: dir_ino=%llu, name=%.*s\n",
		  dir_ino, name_len, name);

	/* Build omap key */
	cfs_make_dentry_key(key, sizeof(key), name, name_len);

	/* Build object ID */
	cfs_meta_oid(&oid, dir_ino);

	/* Calculate metadata length */
	dmeta_len = cfs_dentry_metadata_len(name_len, le32_to_cpu(dmeta->symlink_len));

	/* Write updated metadata */
	ret = cfs_omap_write(fsi, &oid, key, dmeta, dmeta_len);
	if (ret) {
		cfs_debug("update_dentry: failed: %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * Get inode from VFS inode cache or load from RADOS
 */
struct inode *cfs_iget(struct super_block *sb, u64 ino,
		       struct cfs_dentry_metadata *dmeta)
{
	struct cfs_fs_info *fsi = CFS_SB(sb);
	struct inode *inode;
	struct cfs_inode_info *ci;
	int ret;

	cfs_debug("cfs_iget: ino=%llu\n", ino);

	/* Try to find in inode cache first */
	inode = ilookup5(sb, ino, cfs_ino_eq, &ino);
	if (inode) {
		cfs_debug("cfs_iget: found in cache\n");
		return inode;
	}

	/* Need to create new inode */
	inode = cfs_alloc_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	ci = CFS_I(inode);

	/* Initialize inode from metadata */
	if (dmeta) {
		cfs_inode_init(inode, dmeta);
	} else {
		/* Load metadata from RADOS */
		struct ceph_object_id oid;
		char key[64];
		size_t value_len;

		/* For root directory, initialize statically */
		if (ino == CFS_ROOT_INO) {
			inode->i_ino = ino;
			inode->i_sb = sb;
			inode_init_owner(&init_user_ns, inode, NULL, S_IFDIR | 0755);
			i_size_write(inode, 0);
			set_nlink(inode, 2);
			inode->i_atime = inode->i_mtime = current_time(inode);
			inode_set_ctime_current(inode);
			inode->i_op = &cfs_dir_inode_ops;
			inode->i_fop = &cfs_dir_file_ops;
			inode->i_mapping->a_ops = &cfs_aops;
			insert_inode_hash(inode);
			return inode;
		}

		/* Try to load from parent directory's omap */
		cfs_err("cfs_iget: need dmeta to load inode %llu\n", ino);
		cfs_destroy_inode(inode);
		return ERR_PTR(-EINVAL);
	}

	insert_inode_hash(inode);
	return inode;
}

/*
 * Initialize inode from dentry metadata
 */
int cfs_inode_init(struct inode *inode, struct cfs_dentry_metadata *dmeta)
{
	struct cfs_inode_info *ci = CFS_I(inode);
	struct timespec64 ts;
	const char *name;
	unsigned int name_len;

	ci->i_ino = le64_to_cpu(dmeta->ino);
	ci->i_parent_ino = le64_to_cpu(dmeta->parent_ino);
	inode->i_ino = ci->i_ino;

	inode->i_mode = le32_to_cpu(dmeta->mode);
	i_size_write(inode, le64_to_cpu(dmeta->size));
	ci->i_blocks = le64_to_cpu(dmeta->blocks);
	set_nlink(inode, le32_to_cpu(dmeta->nlink));

	inode->i_uid = make_kuid(&init_user_ns, le32_to_cpu(dmeta->uid));
	inode->i_gid = make_kgid(&init_user_ns, le32_to_cpu(dmeta->gid));

	ts.tv_sec = le32_to_cpu(dmeta->atime.tv_sec);
	ts.tv_nsec = le32_to_cpu(dmeta->atime.tv_nsec);
	inode->i_atime = ts;

	ts.tv_sec = le32_to_cpu(dmeta->mtime.tv_sec);
	ts.tv_nsec = le32_to_cpu(dmeta->mtime.tv_nsec);
	inode->i_mtime = ts;

	ts.tv_sec = le32_to_cpu(dmeta->ctime.tv_sec);
	ts.tv_nsec = le32_to_cpu(dmeta->ctime.tv_nsec);
	inode_set_ctime(inode, ts.tv_sec, ts.tv_nsec);

	inode->i_rdev = le64_to_cpu(dmeta->rdev);

	/* Set operations based on file type */
	if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &cfs_dir_inode_ops;
		inode->i_fop = &cfs_dir_file_ops;
		ci->i_dir_count = 0;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_op = &cfs_file_inode_ops;
		inode->i_fop = &cfs_file_ops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &cfs_file_inode_ops;
		/* Symlink target stored after name in dmeta */
		name = (const char *)(dmeta + 1);
		name_len = le32_to_cpu(dmeta->name_len);
		if (dmeta->symlink_len > 0) {
			inode->i_link = kstrndup(name + name_len,
					      le32_to_cpu(dmeta->symlink_len),
					      GFP_KERNEL);
			if (!inode->i_link)
				return -ENOMEM;
		}
	}

	inode->i_mapping->a_ops = &cfs_aops;

	return 0;
}

/*
 * Lookup inode in directory
 */
static struct dentry *cfs_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct cfs_fs_info *fsi = CFS_SB(dir->i_sb);
	struct cfs_inode_info *dir_ci = CFS_I(dir);
	struct cfs_dentry_metadata dmeta;
	struct inode *inode = NULL;
	int ret;

	cfs_debug("cfs_lookup: dir_ino=%llu, name=%.*s\n",
		  dir_ci->i_ino, dentry->d_name.len, dentry->d_name.name);

	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	/* Lookup in parent's omap */
	ret = cfs_lookup_dentry(fsi, dir_ci->i_ino,
			     dentry->d_name.name, dentry->d_name.len,
			     &dmeta);
	if (ret == 0) {
		/* Found, get inode */
		inode = cfs_iget(dir->i_sb, le64_to_cpu(dmeta.ino), &dmeta);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	} else if (ret != -ENOENT) {
		return ERR_PTR(ret);
	}

	/* Negative dentry is valid */
	return d_splice_alias(inode, dentry);
}

/*
 * Create a new file
 */
static int cfs_create(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode, bool excl)
{
	struct cfs_fs_info *fsi = CFS_SB(dir->i_sb);
	struct cfs_inode_info *dir_ci = CFS_I(dir);
	struct cfs_dentry_metadata dmeta;
	int new_ino;
	struct inode *inode;
	int ret;

	cfs_debug("cfs_create: dir_ino=%llu, name=%.*s, mode=%o\n",
		  dir_ci->i_ino, dentry->d_name.len, dentry->d_name.name, mode);

	/* Allocate new inode number */
	new_ino = cfs_alloc_ino(fsi);
	if (new_ino < 0)
		return new_ino;

	/* Initialize metadata */
	memset(&dmeta, 0, sizeof(dmeta));
	dmeta.struct_v = CFS_DENTRY_METADATA_V1;
	dmeta.struct_compat = CFS_DENTRY_METADATA_COMPAT;
	dmeta.ino = cpu_to_le64(new_ino);
	dmeta.parent_ino = cpu_to_le64(dir_ci->i_ino);
	dmeta.mode = cpu_to_le32(S_IFREG | mode);
	dmeta.size = 0;
	dmeta.blocks = 0;
	dmeta.nlink = cpu_to_le32(1);
	dmeta.uid = cpu_to_le32(from_kuid(&init_user_ns, current_fsuid()));
	dmeta.gid = cpu_to_le32(from_kgid(&init_user_ns, current_fsgid()));
	dmeta.name_len = cpu_to_le32(dentry->d_name.len);
	memcpy((void *)&dmeta + offsetof(struct cfs_dentry_metadata, name_len) + 4,
	       dentry->d_name.name, dentry->d_name.len);

	/* Set timestamps */
	struct timespec64 now = current_time(dir);
	dmeta.mtime.tv_sec = cpu_to_le32(now.tv_sec);
	dmeta.mtime.tv_nsec = cpu_to_le32(now.tv_nsec);
	dmeta.atime = dmeta.mtime;
	dmeta.ctime = dmeta.mtime;

	/* Create directory entry */
	ret = cfs_create_dentry(fsi, dir_ci->i_ino,
			     dentry->d_name.name, dentry->d_name.len,
			     &dmeta, excl);
	if (ret)
		return ret;

	/* Create VFS inode */
	inode = cfs_iget(dir->i_sb, new_ino, &dmeta);
	if (IS_ERR(inode)) {
		cfs_remove_dentry(fsi, dir_ci->i_ino,
			       dentry->d_name.name, dentry->d_name.len);
		return PTR_ERR(inode);
	}

	/* Update directory */
	spin_lock(&dir->i_lock);
	dir->i_mtime = inode->i_mtime;
	inode_set_ctime_current(dir);
	inc_nlink(dir);
	spin_unlock(&dir->i_lock);

	d_instantiate(dentry, inode);
	return 0;
}

/*
 * Link a file
 */
static int cfs_link(struct dentry *old_dentry, struct inode *dir,
		    struct dentry *dentry)
{
	struct cfs_fs_info *fsi = CFS_SB(dir->i_sb);
	struct cfs_inode_info *dir_ci = CFS_I(dir);
	struct inode *inode = d_inode(old_dentry);
	struct cfs_inode_info *ci = CFS_I(inode);
	struct cfs_dentry_metadata dmeta;
	int ret;

	cfs_debug("cfs_link: dir_ino=%llu, name=%.*s, target_ino=%llu\n",
		  dir_ci->i_ino, dentry->d_name.len, dentry->d_name.name,
		  ci->i_ino);

	/* Build dentry metadata */
	memset(&dmeta, 0, sizeof(dmeta));
	cfs_inode_to_dentry(inode, &dmeta);
	dmeta.name_len = cpu_to_le32(dentry->d_name.len);
	memcpy((void *)&dmeta + offsetof(struct cfs_dentry_metadata, name_len) + 4,
	       dentry->d_name.name, dentry->d_name.len);

	/* Create directory entry */
	ret = cfs_create_dentry(fsi, dir_ci->i_ino,
			     dentry->d_name.name, dentry->d_name.len,
			     &dmeta, true);
	if (ret)
		return ret;

	/* Update inode */
	spin_lock(&inode->i_lock);
	inc_nlink(inode);
	inode->i_ctime = current_time(inode);
	spin_unlock(&inode->i_lock);

	/* Update directory */
	spin_lock(&dir->i_lock);
	dir->i_mtime = inode->i_ctime;
	inode_set_ctime_current(dir);
	spin_unlock(&dir->i_lock);

	d_instantiate(dentry, inode);
	ihold(inode);
	return 0;
}

/*
 * Unlink a file
 */
static int cfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct cfs_fs_info *fsi = CFS_SB(dir->i_sb);
	struct cfs_inode_info *dir_ci = CFS_I(dir);
	struct inode *inode = d_inode(dentry);
	struct cfs_inode_info *ci = CFS_I(inode);
	struct timespec64 now;
	int ret;

	cfs_debug("cfs_unlink: dir_ino=%llu, name=%.*s, target_ino=%llu\n",
		  dir_ci->i_ino, dentry->d_name.len, dentry->d_name.name,
		  ci->i_ino);

	/* Remove directory entry */
	ret = cfs_remove_dentry(fsi, dir_ci->i_ino,
			     dentry->d_name.name, dentry->d_name.len);
	if (ret)
		return ret;

	/* Update inode */
	spin_lock(&inode->i_lock);
	drop_nlink(inode);
	now = current_time(inode);
	inode_set_ctime(inode, now.tv_sec, now.tv_nsec);
	spin_unlock(&inode->i_lock);

	/* Update directory */
	spin_lock(&dir->i_lock);
	dir->i_mtime = now;
	inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
	spin_unlock(&dir->i_lock);

	return 0;
}

/*
 * Symlink
 */
static int cfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, const char *symname)
{
	struct cfs_fs_info *fsi = CFS_SB(dir->i_sb);
	struct cfs_inode_info *dir_ci = CFS_I(dir);
	struct cfs_dentry_metadata *dmeta;
	int new_ino;
	unsigned int name_len = dentry->d_name.len;
	unsigned int sym_len = strlen(symname);
	size_t dmeta_size;
	struct inode *inode;
	struct timespec64 now;
	int ret;

	cfs_debug("cfs_symlink: dir_ino=%llu, name=%.*s, target=%s\n",
		  dir_ci->i_ino, name_len, dentry->d_name.name, symname);

	/* Allocate inode */
	new_ino = cfs_alloc_ino(fsi);
	if (new_ino < 0)
		return new_ino;

	/* Allocate metadata buffer */
	dmeta_size = cfs_dentry_metadata_len(name_len, sym_len);
	dmeta = kzalloc(dmeta_size, GFP_KERNEL);
	if (!dmeta)
		return -ENOMEM;

	/* Initialize metadata */
	dmeta->struct_v = CFS_DENTRY_METADATA_V1;
	dmeta->struct_compat = CFS_DENTRY_METADATA_COMPAT;
	dmeta->ino = cpu_to_le64(new_ino);
	dmeta->parent_ino = cpu_to_le64(dir_ci->i_ino);
	dmeta->mode = cpu_to_le32(S_IFLNK | S_IRWXUGO);
	dmeta->size = cpu_to_le64(sym_len);
	dmeta->blocks = 0;
	dmeta->nlink = cpu_to_le32(1);
	dmeta->uid = cpu_to_le32(from_kuid(&init_user_ns, current_fsuid()));
	dmeta->gid = cpu_to_le32(from_kgid(&init_user_ns, current_fsgid()));
	dmeta->name_len = cpu_to_le32(name_len);
	dmeta->symlink_len = cpu_to_le32(sym_len);

	now = current_time(dir);
	dmeta->mtime.tv_sec = cpu_to_le32(now.tv_sec);
	dmeta->mtime.tv_nsec = cpu_to_le32(now.tv_nsec);
	dmeta->atime = dmeta->mtime;
	dmeta->ctime = dmeta->mtime;

	/* Copy name and symlink target */
	memcpy((void *)dmeta + sizeof(*dmeta), dentry->d_name.name, name_len);
	memcpy((void *)dmeta + sizeof(*dmeta) + name_len, symname, sym_len);

	/* Create directory entry */
	ret = cfs_create_dentry(fsi, dir_ci->i_ino,
			     dentry->d_name.name, name_len, dmeta, true);
	if (ret) {
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

	/* Update directory */
	spin_lock(&dir->i_lock);
	dir->i_mtime = now;
	inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
	spin_unlock(&dir->i_lock);

	d_instantiate(dentry, inode);
	return 0;
}

/*
 * Get symlink target
 */
static const char *cfs_get_link(struct dentry *dentry, struct inode *inode,
				struct delayed_call *done)
{
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("cfs_get_link: ino=%llu\n", ci->i_ino);

	if (!dentry)
		return ERR_PTR(-ECHILD);

	return inode->i_link;
}

/*
 * File inode operations
 */
const struct inode_operations cfs_file_inode_ops = {
	.setattr       = simple_setattr,
	.getattr       = simple_getattr,
};

/*
 * Directory inode operations
 */
const struct inode_operations cfs_dir_inode_ops = {
	.create        = cfs_create,
	.lookup        = cfs_lookup,
	.link          = cfs_link,
	.unlink        = cfs_unlink,
	.symlink       = cfs_symlink,
	.mkdir         = cfs_mkdir,
	.rmdir         = cfs_rmdir,
	.rename        = cfs_rename,
	.getattr       = simple_getattr,
	.setattr       = simple_setattr,
};

/*
 * Symlink inode operations
 */
static const struct inode_operations cfs_symlink_inode_ops = {
	.get_link      = cfs_get_link,
	.getattr       = simple_getattr,
	.setattr       = simple_setattr,
};
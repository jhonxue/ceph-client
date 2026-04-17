/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CFS - Ceph File System Simple
 *
 * Superblock operations and filesystem registration
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/statfs.h>
#include <linux/mount.h>
#include <linux/ceph/ceph_client.h>
#include <linux/ceph/ceph_features.h>
#include <linux/ceph/osdmap.h>

#include "super.h"
#include "internal.h"
#include "rados.h"

#define CFS_AUTHOR     "CFS Team"
#define CFS_DESC       "CFS - Simple Ceph-backed File System"

MODULE_AUTHOR(CFS_AUTHOR);
MODULE_DESCRIPTION(CFS_DESC);
MODULE_LICENSE("GPL");

/* Mount option parsing */
enum cfs_param {
	Opt_mon_addr,
	Opt_meta_pool,
	Opt_data_pool,
	Opt_mode,
	Opt_rsize,
	Opt_wsize,
};

static const struct fs_parameter_spec cfs_fs_parameters[] = {
	fsparam_string("mon_addr", Opt_mon_addr),
	fsparam_string("meta_pool", Opt_meta_pool),
	fsparam_string("data_pool", Opt_data_pool),
	fsparam_u32oct("mode", Opt_mode),
	fsparam_u32("rsize", Opt_rsize),
	fsparam_u32("wsize", Opt_wsize),
	{}
};

/*
 * Parse mount options
 */
static int cfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct cfs_fs_info *fsi = fc->s_fs_info;
	struct fs_parse_result result;
	int opt, ret;

	opt = fs_parse(fc, cfs_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_mon_addr:
		kfree(fsi->opts->mon_addr);
		fsi->opts->mon_addr = kstrdup(param->string, GFP_KERNEL);
		if (!fsi->opts->mon_addr)
			return -ENOMEM;
		break;
	case Opt_meta_pool:
		kfree(fsi->opts->meta_pool);
		fsi->opts->meta_pool = kstrdup(param->string, GFP_KERNEL);
		if (!fsi->opts->meta_pool)
			return -ENOMEM;
		break;
	case Opt_data_pool:
		kfree(fsi->opts->data_pool);
		fsi->opts->data_pool = kstrdup(param->string, GFP_KERNEL);
		if (!fsi->opts->data_pool)
			return -ENOMEM;
		break;
	case Opt_mode:
		fsi->opts->mode = result.uint_32 & S_IALLUGO;
		break;
	case Opt_rsize:
		fsi->opts->rsize = result.uint_32;
		break;
	case Opt_wsize:
		fsi->opts->wsize = result.uint_32;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * Super block statfs
 */
static int cfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct cfs_fs_info *fsi = CFS_SB(sb);
	struct ceph_client *client = fsi->client;
	u64 total, used, avail;
	int ret;

	buf->f_type = CFS_SUPER_MAGIC;
	buf->f_bsize = CFS_BLOCK_SIZE;
	buf->f_namelen = NAME_MAX;

	/* Query cluster statistics via OSD client */
	ret = ceph_get_pool_stats(client, fsi->data_pool_id,
				   &total, &used, &avail);
	if (ret) {
		cfs_debug("statfs: failed to get pool stats: %d\n", ret);
		/* Return reasonable defaults */
		buf->f_blocks = 0;
		buf->f_bfree = 0;
		buf->f_bavail = 0;
		buf->f_files = 0;
		buf->f_ffree = 0;
		return 0;
	}

	buf->f_blocks = total >> CFS_BLOCK_SHIFT;
	buf->f_bfree = avail >> CFS_BLOCK_SHIFT;
	buf->f_bavail = avail >> CFS_BLOCK_SHIFT;
	buf->f_files = 0;  /* Unknown */
	buf->f_ffree = 0;  /* Unknown */

	return 0;
}

/*
 * Write superblock (sync)
 */
static int cfs_sync_fs(struct super_block *sb, int wait)
{
	/* CFS is always in sync with RADOS */
	return 0;
}

/*
 * Put super - cleanup on unmount
 */
static void cfs_put_super(struct super_block *sb)
{
	struct cfs_fs_info *fsi = CFS_SB(sb);

	if (!fsi)
		return;

	cfs_debug("put_super: cleaning up\n");

	/* Cleanup RADOS resources */
	cfs_rados_cleanup(fsi);

	/* Free mount options */
	if (fsi->opts) {
		kfree(fsi->opts->mon_addr);
		kfree(fsi->opts->meta_pool);
		kfree(fsi->opts->data_pool);
		kfree(fsi->opts);
	}

	/* Free client */
	if (fsi->client)
		ceph_destroy_client(fsi->client);

	kfree(fsi);
	sb->s_fs_info = NULL;
}

/*
 * Remount filesystem
 */
static int cfs_remount(struct super_block *sb, int *flags, char *data)
{
	sync_filesystem(sb);
	return 0;
}

/*
 * Show mount options
 */
static int cfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct cfs_fs_info *fsi = CFS_SB(root->d_sb);

	if (fsi->opts->mon_addr)
		seq_printf(m, ",mon_addr=%s", fsi->opts->mon_addr);
	if (fsi->opts->meta_pool)
		seq_printf(m, ",meta_pool=%s", fsi->opts->meta_pool);
	if (fsi->opts->data_pool)
		seq_printf(m, ",data_pool=%s", fsi->opts->data_pool);
	if (fsi->opts->mode != 0755)
		seq_printf(m, ",mode=%o", fsi->opts->mode);

	return 0;
}

/*
 * Allocate inode
 */
static struct inode *cfs_alloc_inode(struct super_block *sb)
{
	struct cfs_inode_info *ci;

	ci = kmem_cache_zalloc(cfs_inode_cachep, GFP_NOFS);
	if (!ci)
		return NULL;

	spin_lock_init(&ci->i_lock);
	ci->i_ino = 0;
	ci->i_parent_ino = 0;
	ci->i_dir_count = 0;

	/*
	 * Initialize netfs context with CFS netfs ops.
	 * Netfs is always enabled for CFS - it provides the core I/O path.
	 */
	netfs_inode_init(&ci->netfs, &cfs_netfs_ops, false);

	return &ci->netfs.inode;
}

/*
 * Destroy inode
 */
static void cfs_destroy_inode(struct inode *inode)
{
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("destroy_inode: ino=%llu\n", ci->i_ino);
	kmem_cache_free(cfs_inode_cachep, ci);
}

/*
 * Update netfs context for the root inode.
 * This ensures remote_i_size is correctly set after mount.
 * Netfs ops are already set in cfs_alloc_inode().
 */
void cfs_update_root_inode_netfs(struct super_block *sb)
{
	struct inode *root_inode;
	struct cfs_inode_info *ci;

	/* Get the root inode - the only inode at mount time */
	root_inode = sb->s_root->d_inode;
	if (!root_inode)
		return;

	ci = CFS_I(root_inode);

	/* Update remote_i_size to match current i_size */
	ci->netfs.remote_i_size = i_size_read(root_inode);
}

/*
 * Drop inode
 */
static void cfs_drop_inode(struct inode *inode)
{
	generic_drop_inode(inode);
}

/*
 * Evict inode
 */
static void cfs_evict_inode(struct inode *inode)
{
	struct cfs_fs_info *fsi = CFS_SB(inode->i_sb);
	struct cfs_inode_info *ci = CFS_I(inode);

	cfs_debug("evict_inode: ino=%llu\n", ci->i_ino);

	/* Delete data objects for regular files */
	if (S_ISREG(inode->i_mode) && !is_bad_inode(inode)) {
		/* TODO: Mark data objects for deletion */
	}

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

/*
 * Superblock operations
 */
static const struct super_operations cfs_super_ops = {
	.alloc_inode    = cfs_alloc_inode,
	.destroy_inode  = cfs_destroy_inode,
	.drop_inode     = cfs_drop_inode,
	.evict_inode    = cfs_evict_inode,
	.put_super      = cfs_put_super,
	.statfs         = cfs_statfs,
	.sync_fs        = cfs_sync_fs,
	.remount_fs     = cfs_remount,
	.show_options   = cfs_show_options,
};

/* Inode cache */
struct kmem_cache *cfs_inode_cachep;

static int cfs_init_inode_cache(void)
{
	cfs_inode_cachep = kmem_cache_create("cfs_inode_cache",
					       sizeof(struct cfs_inode_info),
					       0, SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD,
					       NULL);
	if (!cfs_inode_cachep)
		return -ENOMEM;

	return 0;
}

static void cfs_destroy_inode_cache(void)
{
	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(cfs_inode_cachep);
}

/*
 * Initialize root inode
 */
static int cfs_init_root_inode(struct super_block *sb)
{
	struct inode *root_inode;
	struct cfs_inode_info *ci;
	struct cfs_dentry_metadata dmeta;
	struct timespec64 now;

	/* Create root inode (ino = 1) */
	root_inode = cfs_alloc_inode(sb);
	if (!root_inode)
		return -ENOMEM;

	ci = CFS_I(root_inode);
	ci->i_ino = CFS_ROOT_INO;
	ci->i_parent_ino = 0;  /* Root has no parent */

	/* Initialize inode */
	root_inode->i_ino = CFS_ROOT_INO;
	root_inode->i_sb = sb;
	inode_init_owner(&init_user_ns, root_inode, NULL, S_IFDIR | 0755);

	now = current_time(root_inode);
	root_inode->i_atime = now;
	root_inode->i_mtime = now;
	inode_set_ctime_current(root_inode);

	i_size_write(root_inode, 0);
	set_nlink(root_inode, 2);

	/* Set inode operations */
	inode_init_always(sb, root_inode);
	root_inode->i_op = &cfs_dir_inode_ops;
	root_inode->i_fop = &cfs_dir_file_ops;
	root_inode->i_mapping->a_ops = &cfs_aops;

	/* Insert into inode cache */
	insert_inode_hash(root_inode);

	/* Create root dentry */
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root)
		return -ENOMEM;

	/* Initialize metadata for root directory in RADOS */
	memset(&dmeta, 0, sizeof(dmeta));
	dmeta.struct_v = CFS_DENTRY_METADATA_V1;
	dmeta.struct_compat = CFS_DENTRY_METADATA_COMPAT;
	dmeta.ino = cpu_to_le64(CFS_ROOT_INO);
	dmeta.parent_ino = 0;
	dmeta.mode = cpu_to_le32(S_IFDIR | 0755);
	dmeta.size = 0;
	dmeta.blocks = 0;
	dmeta.atime.tv_sec = cpu_to_le32(now.tv_sec);
	dmeta.atime.tv_nsec = cpu_to_le32(now.tv_nsec);
	dmeta.mtime.tv_sec = cpu_to_le32(now.tv_sec);
	dmeta.mtime.tv_nsec = cpu_to_le32(now.tv_nsec);
	dmeta.ctime.tv_sec = cpu_to_le32(now.tv_sec);
	dmeta.ctime.tv_nsec = cpu_to_le32(now.tv_nsec);
	dmeta.nlink = cpu_to_le32(2);
	dmeta.uid = cpu_to_le32(from_kuid(&init_user_ns, root_inode->i_uid));
	dmeta.gid = cpu_to_le32(from_kgid(&init_user_ns, root_inode->i_gid));

	return 0;
}

/*
 * Fill superblock
 */
static int cfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct cfs_fs_info *fsi = sb->s_fs_info;
	int ret;

	cfs_debug("fill_super: starting\n");

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic = CFS_SUPER_MAGIC;
	sb->s_op = &cfs_super_ops;
	sb->s_time_gran = 1;

	/* Connect to RADOS cluster */
	ret = cfs_rados_init(fsi);
	if (ret) {
		cfs_err("failed to initialize RADOS: %d\n", ret);
		return ret;
	}

	/* Connect to pools */
	ret = cfs_connect_pools(fsi);
	if (ret) {
		cfs_err("failed to connect to pools: %d\n", ret);
		goto err_rados_cleanup;
	}

	/* Initialize inode allocator */
	ret = cfs_init_ino_allocator(fsi);
	if (ret) {
		cfs_err("failed to initialize inode allocator: %d\n", ret);
		goto err_rados_cleanup;
	}

	/* Create root inode */
	ret = cfs_init_root_inode(sb);
	if (ret) {
		cfs_err("failed to create root inode: %d\n", ret);
		goto err_rados_cleanup;
	}

	/*
	 * Update netfs context for root inode.
	 * Netfs ops are already set in cfs_alloc_inode().
	 */
	cfs_update_root_inode_netfs(sb);

	cfs_info("filesystem mounted successfully\n");
	return 0;

err_rados_cleanup:
	cfs_rados_cleanup(fsi);
	return ret;
}

/*
 * Free fs_context
 */
static void cfs_free_fc(struct fs_context *fc)
{
	struct cfs_fs_info *fsi = fc->s_fs_info;

	if (fsi) {
		if (fsi->opts) {
			kfree(fsi->opts->mon_addr);
			kfree(fsi->opts->meta_pool);
			kfree(fsi->opts->data_pool);
			kfree(fsi->opts);
		}
		kfree(fsi);
	}
}

/*
 * Get tree (mount)
 */
static int cfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, cfs_fill_super);
}

/*
 * Reconfigure (remount)
 */
static int cfs_reconfigure(struct fs_context *fc)
{
	struct super_block *sb = fc->root->d_sb;
	sync_filesystem(sb);
	return 0;
}

static const struct fs_context_operations cfs_fs_context_ops = {
	.free           = cfs_free_fc,
	.parse_param    = cfs_parse_param,
	.get_tree       = cfs_get_tree,
	.reconfigure    = cfs_reconfigure,
};

/*
 * Initialize filesystem context
 */
int cfs_init_fs_context(struct fs_context *fc)
{
	struct cfs_fs_info *fsi;
	struct cfs_mount_options *opts;

	fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
	if (!fsi)
		return -ENOMEM;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts) {
		kfree(fsi);
		return -ENOMEM;
	}

	/* Default options */
	opts->mon_addr = NULL;
	opts->meta_pool = kstrdup("cfs_meta", GFP_KERNEL);
	opts->data_pool = kstrdup("cfs_data", GFP_KERNEL);
	opts->ino_batch_size = CFS_INO_BATCH_SIZE;
	opts->mode = 0755;

	fsi->opts = opts;
	spin_lock_init(&fsi->lock);
	mutex_init(&fsi->mount_mutex);
	fsi->mount_state = CFS_MOUNT_UNMOUNTED;
	fsi->ino_alloc.initialized = false;
	spin_lock_init(&fsi->ino_alloc.lock);

	fc->s_fs_info = fsi;
	fc->ops = &cfs_fs_context_ops;

	return 0;
}

/*
 * Kill superblock
 */
void cfs_kill_sb(struct super_block *sb)
{
	kill_litter_super(sb);
}

static struct file_system_type cfs_fs_type = {
	.owner          = THIS_MODULE,
	.name           = "cfs",
	.init_fs_context = cfs_init_fs_context,
	.kill_sb        = cfs_kill_sb,
	.fs_flags       = FS_USERNS_MOUNT,
};

/*
 * Module initialization
 */
static int __init init_cfs(void)
{
	int ret;

	ret = cfs_init_inode_cache();
	if (ret) {
		cfs_err("failed to create inode cache: %d\n", ret);
		return ret;
	}

	ret = register_filesystem(&cfs_fs_type);
	if (ret) {
		cfs_err("failed to register filesystem: %d\n", ret);
		goto err_cache;
	}

	cfs_info("CFS filesystem driver registered\n");
	return 0;

err_cache:
	cfs_destroy_inode_cache();
	return ret;
}

/*
 * Module cleanup
 */
static void __exit exit_cfs(void)
{
	unregister_filesystem(&cfs_fs_type);
	cfs_destroy_inode_cache();
	cfs_info("CFS filesystem driver unregistered\n");
}

module_init(init_cfs);
module_exit(exit_cfs);
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FS_CFS_SUPER_H
#define _FS_CFS_SUPER_H

#include <linux/ceph/osd_client.h>
#include <linux/ceph/ceph_hash.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/kref.h>

/*
 * CFS - Ceph File System Simple
 * 
 * A simple file system that stores metadata in RADOS omap
 * and file data in RADOS objects with 4MB slicing.
 */

/* Magic number for CFS */
#define CFS_SUPER_MAGIC     0xCFCF5353  /* "CFSS" */

/* Data block constants */
#define CFS_BLOCK_SIZE      (4 * 1024 * 1024)  /* 4MB */
#define CFS_BLOCK_SHIFT     22                  /* log2(4MB) */

/* Inode constants */
#define CFS_ROOT_INO        1                   /* Root directory inode */
#define CFS_MIN_USER_INO    100                 /* First user inode */
#define CFS_INO_BATCH_SIZE  100                 /* Inode batch size for allocation */

/* Allocator object */
#define CFS_INODE_ALLOCATOR_OID  "inode_allocator"
#define CFS_INODE_ALLOCATOR_KEY  "next_ino"

/* Hash type for directory entry keys */
#define CFS_HASH_TYPE       CEPH_STR_HASH_RJENKINS

/*
 * Directory entry metadata stored in omap
 * omap key: "hash_hex_filename" (e.g., "a3b2c1d0_file.txt")
 */
struct cfs_dentry_metadata {
	__u8 struct_v;              /* Structure version = 1 */
	__u8 struct_compat;         /* Compatible version = 1 */
	__le32 struct_len;          /* Structure length */

	/* File identification */
	__le64 ino;                 /* Inode number (unique identifier) */
	__le32 name_len;            /* Filename length */
	/* char name[] follows */    /* Filename (variable length) */

	/* Inode attributes */
	__le32 mode;                /* File type and permissions */
	__le64 size;                /* File size */
	__le64 blocks;              /* Number of data blocks */

	/* Timestamps */
	struct ceph_timespec atime; /* Access time */
	struct ceph_timespec mtime; /* Modification time */
	struct ceph_timespec ctime; /* Change time */

	/* User/group info */
	__le32 nlink;               /* Link count */
	__le32 uid;                 /* User ID */
	__le32 gid;                 /* Group ID */

	/* Special file info */
	__le64 parent_ino;          /* Parent directory inode */
	__le64 rdev;                /* Device number (for special files) */
	__le32 symlink_len;         /* Symlink target length */
	/* char symlink_target[] follows if symlink */
} __packed;

/*
 * Inode allocator data stored in RADOS omap
 */
struct cfs_allocator_data {
	__u8 struct_v;              /* Structure version = 1 */
	__u8 struct_compat;         /* Compatible version = 1 */
	__le32 struct_len;          /* Structure length */
	__le64 next_ino;            /* Next available inode number */
	__le64 version;             /* Allocator version (incremented on each update) */
} __packed;

/*
 * Local inode allocator state
 */
struct cfs_ino_allocator {
	u64 local_next;              /* Next inode in local batch */
	u64 local_end;               /* End of local batch (exclusive) */
	u64 global_version;          /* Last seen global version */
	spinlock_t lock;             /* Local allocation lock */
	bool initialized;            /* Allocator initialized flag */
};

/*
 * CFS mount options
 */
struct cfs_mount_options {
	char *mon_addr;              /* Monitor address string */
	char *meta_pool;             /* Metadata pool name */
	char *data_pool;             /* Data pool name */
	unsigned int ino_batch_size; /* Inode batch size */
	umode_t mode;                /* Default file mode */
};

/*
 * CFS file system info (per-mount instance)
 */
struct cfs_fs_info {
	struct super_block *sb;      /* VFS superblock */
	struct ceph_client *client;  /* Ceph client */
	struct ceph_osd_client *osdc; /* OSD client reference */
	
	s64 meta_pool_id;            /* Metadata pool ID */
	s64 data_pool_id;            /* Data pool ID */

	struct cfs_mount_options *opts; /* Mount options */
	struct cfs_ino_allocator ino_alloc; /* Inode allocator */

	/* Mount state */
	int mount_state;
	bool mounted;
	
	/* Locks */
	struct mutex mount_mutex;    /* Mount operation mutex */
	spinlock_t lock;             /* General lock */
};

/*
 * CFS inode info (embedded in VFS inode)
 */
struct cfs_inode_info {
	u64 i_ino;                   /* Inode number */
	u64 i_parent_ino;            /* Parent directory inode */
	umode_t i_mode;              /* File mode */
	u64 i_size;                  /* File size */
	u64 i_blocks;                /* Number of data blocks */
	u32 i_nlink;                 /* Link count */
	kuid_t i_uid;                /* User ID */
	kgid_t i_gid;                /* Group ID */
	struct timespec64 i_atime;   /* Access time */
	struct timespec64 i_mtime;   /* Modification time */
	struct timespec64 i_ctime;   /* Change time */
	dev_t i_rdev;                /* Device number */
	
	/* For directories: child count cache */
	u32 i_dir_count;
	
	/* Lock for inode operations */
	spinlock_t i_lock;
	
	/* VFS inode embedded at end */
	struct inode vfs_inode;
};

/*
 * CFS directory entry info (for readdir)
 */
struct cfs_dir_entry {
	u64 ino;
	char *name;
	unsigned int name_len;
	umode_t mode;
};

/* Inline functions for inode conversion */
static inline struct cfs_inode_info *CFS_I(struct inode *inode)
{
	return container_of(inode, struct cfs_inode_info, vfs_inode);
}

static inline struct inode *CFS_INODE(struct cfs_inode_info *ci)
{
	return &ci->vfs_inode;
}

static inline struct cfs_fs_info *CFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* Block calculation functions */
static inline u32 cfs_part_num(u64 offset)
{
	return offset >> CFS_BLOCK_SHIFT;
}

static inline u64 cfs_part_offset(u64 offset)
{
	return offset & (CFS_BLOCK_SIZE - 1);
}

static inline u64 cfs_part_remaining(u64 offset, u64 length)
{
	u64 part_off = cfs_part_offset(offset);
	u64 remaining = CFS_BLOCK_SIZE - part_off;
	return min(length, remaining);
}

static inline u32 cfs_max_part(u64 size)
{
	if (size == 0)
		return 0;
	return cfs_part_num(size - 1) + 1;
}

/* Directory entry key generation */
static inline void cfs_make_dentry_key(char *key_buf, size_t buf_size,
					const char *name, unsigned int name_len)
{
	unsigned int hash = ceph_str_hash(CFS_HASH_TYPE, name, name_len);
	snprintf(key_buf, buf_size, "%08x_%s", hash, name);
}

/* Object ID construction */
static inline void cfs_meta_oid(struct ceph_object_id *oid, u64 ino)
{
	ceph_oid_printf(oid, "%llu", ino);
}

static inline void cfs_data_oid(struct ceph_object_id *oid, u64 ino, u32 part)
{
	ceph_oid_printf(oid, "%llu.%u", ino, part);
}

static inline void cfs_allocator_oid(struct ceph_object_id *oid)
{
	ceph_oid_set(oid, CFS_INODE_ALLOCATOR_OID);
}

/* Check if inode needs meta object */
static inline bool cfs_needs_meta_object(umode_t mode)
{
	return S_ISDIR(mode);
}

/* Check if inode is root */
static inline bool cfs_is_root_inode(u64 ino)
{
	return ino == CFS_ROOT_INO;
}

/* Encoding/decoding helpers */
static inline void cfs_encode_u64(void **p, u64 val)
{
	*(__le64 *)*p = cpu_to_le64(val);
	*p += sizeof(__le64);
}

static inline void cfs_encode_u32(void **p, u32 val)
{
	*(__le32 *)*p = cpu_to_le32(val);
	*p += sizeof(__le32);
}

static inline void cfs_encode_u8(void **p, u8 val)
{
	*(__u8 *)*p = val;
	*p += sizeof(__u8);
}

static inline void cfs_encode_string(void **p, void *end,
				     const char *str, unsigned int len)
{
	cfs_encode_u32(p, len);
	memcpy(*p, str, len);
	*p += len;
}

static inline void cfs_encode_timespec(void **p, struct timespec64 *ts)
{
	struct ceph_timespec cts;
	cts.tv_sec = cpu_to_le32(ts->tv_sec);
	cts.tv_nsec = cpu_to_le32(ts->tv_nsec);
	memcpy(*p, &cts, sizeof(cts));
	*p += sizeof(cts);
}

static inline u64 cfs_decode_u64(void **p)
{
	u64 val = le64_to_cpu(*(__le64 *)*p);
	*p += sizeof(__le64);
	return val;
}

static inline u32 cfs_decode_u32(void **p)
{
	u32 val = le32_to_cpu(*(__le32 *)*p);
	*p += sizeof(__le32);
	return val;
}

static inline u8 cfs_decode_u8(void **p)
{
	u8 val = *(__u8 *)*p;
	*p += sizeof(__u8);
	return val;
}

static inline void cfs_decode_timespec(void **p, struct timespec64 *ts)
{
	struct ceph_timespec cts;
	memcpy(&cts, *p, sizeof(cts));
	ts->tv_sec = le32_to_cpu(cts.tv_sec);
	ts->tv_nsec = le32_to_cpu(cts.tv_nsec);
	*p += sizeof(cts);
}

/* Forward declarations for operations */
extern const struct super_operations cfs_super_ops;
extern const struct inode_operations cfs_dir_inode_ops;
extern const struct inode_operations cfs_file_inode_ops;
extern const struct file_operations cfs_dir_file_ops;
extern const struct file_operations cfs_file_ops;
extern const struct address_space_operations cfs_aops;

/* Function declarations */
/* super.c */
int cfs_init_fs_context(struct fs_context *fc);
void cfs_kill_sb(struct super_block *sb);

/* inode.c */
struct inode *cfs_alloc_inode(struct super_block *sb);
void cfs_destroy_inode(struct inode *inode);
struct inode *cfs_iget(struct super_block *sb, u64 ino,
		       struct cfs_dentry_metadata *dmeta);
int cfs_inode_init(struct inode *inode, struct cfs_dentry_metadata *dmeta);

/* rados.c */
int cfs_rados_init(struct cfs_fs_info *fsi);
void cfs_rados_cleanup(struct cfs_fs_info *fsi);
int cfs_connect_pools(struct cfs_fs_info *fsi);

/* Inode allocator */
int cfs_init_ino_allocator(struct cfs_fs_info *fsi);
int cfs_alloc_ino(struct cfs_fs_info *fsi);

/* Metadata operations */
int cfs_lookup_dentry(struct cfs_fs_info *fsi, u64 dir_ino,
		      const char *name, unsigned int name_len,
		      struct cfs_dentry_metadata *dmeta);
int cfs_create_dentry(struct cfs_fs_info *fsi, u64 dir_ino,
		      const char *name, unsigned int name_len,
		      struct cfs_dentry_metadata *dmeta, bool exclusive);
int cfs_remove_dentry(struct cfs_fs_info *fsi, u64 dir_ino,
		      const char *name, unsigned int name_len);
int cfs_update_dentry(struct cfs_fs_info *fsi, u64 dir_ino,
		      const char *name, unsigned int name_len,
		      struct cfs_dentry_metadata *dmeta);
int cfs_list_dentries(struct cfs_fs_info *fsi, u64 dir_ino,
		      struct cfs_dir_entry **entries, u32 *count);

/* Data operations */
int cfs_read_data(struct cfs_fs_info *fsi, u64 ino, u64 offset,
		  u64 length, struct page **pages, int num_pages);
int cfs_write_data(struct cfs_fs_info *fsi, u64 ino, u64 offset,
		   u64 length, struct page **pages, int num_pages,
		   struct timespec64 *mtime);
int cfs_truncate_data(struct cfs_fs_info *fsi, u64 ino,
		      u64 old_size, u64 new_size, struct timespec64 *mtime);
int cfs_delete_data_objects(struct cfs_fs_info *fsi, u64 ino, u32 max_part);

/* dir.c */
int cfs_dir_create(struct cfs_fs_info *fsi, u64 parent_ino,
		   const char *name, umode_t mode, u64 *new_ino);
int cfs_dir_lookup(struct cfs_fs_info *fsi, u64 dir_ino,
		   const char *name, u64 *child_ino);
int cfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
	      struct dentry *dentry, umode_t mode);
int cfs_rmdir(struct inode *dir, struct dentry *dentry);
int cfs_readdir(struct file *file, struct dir_context *ctx);
int cfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
	       struct dentry *old_dentry, struct inode *new_dir,
	       struct dentry *new_dentry, unsigned int flags);
int cfs_dir_list_check_empty(const char *key, void *value, size_t len, void *priv);

/* file.c */
int cfs_file_create(struct mnt_idmap *idmap, struct inode *dir,
		    struct dentry *dentry, umode_t mode, bool excl);
int cfs_file_unlink(struct inode *dir, struct dentry *dentry);
int cfs_file_open(struct inode *inode, struct file *file);
int cfs_file_release(struct inode *inode, struct file *file);
ssize_t cfs_file_read_iter(struct kiocb *iocb, struct iov_iter *iter);
ssize_t cfs_file_write_iter(struct kiocb *iocb, struct iov_iter *iter);

/* addr.c */
int cfs_read_folio(struct file *file, struct folio *folio);
int cfs_write_begin(struct file *file, struct address_space *mapping,
		    loff_t pos, unsigned len, struct folio **foliop,
		    void **fsdata);
int cfs_write_end(struct file *file, struct address_space *mapping,
		  loff_t pos, unsigned len, unsigned copied,
		  struct folio *folio, void *fsdata);

/* inode.c helpers */
int cfs_ino_eq(struct inode *inode, void *data);

/* rados.c omap helpers */
int cfs_omap_list(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		  const char *start_after, const char *prefix,
		  int max_entries,
		  int (*cb)(const char *key, void *value, size_t len, void *priv),
		  void *priv);
int cfs_data_read(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		  u64 offset, u64 length, struct page **pages, int num_pages);
int cfs_data_write(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		   u64 offset, u64 length, struct page **pages, int num_pages);
int cfs_data_delete(struct cfs_fs_info *fsi, struct ceph_object_id *oid);
int cfs_data_truncate(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		      u64 new_size);

#endif /* _FS_CFS_SUPER_H */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FS_CFS_RADOS_H
#define _FS_CFS_RADOS_H

#include "super.h"

/*
 * CFS RADOS Communication Layer
 * 
 * Provides wrapper functions for communicating with RADOS
 * via OSD class methods (metadata) and native OSD operations (data).
 */

/* OSD class name for CFS metadata operations */
#define CFS_CLASS_NAME        "cfs"
#define CFS_CLASS_VERSION     1

/* Method names for CFS OSD class */
#define CFS_METHOD_ALLOC_INO  "alloc_ino"
#define CFS_METHOD_LIST_DIR   "list_dir"
#define CFS_METHOD_VERSION    "version"

/* Maximum retries for operations */
#define CFS_MAX_RETRIES       10

/* Structure for RADOS operation context */
struct cfs_rados_req {
	struct ceph_osd_request *osd_req;
	struct ceph_osd_client *osdc;
	struct completion completion;
	int result;
	void *priv;
};

/* Structure for directory listing */
struct cfs_dir_list_cb {
	struct cfs_dir_entry *entries;
	u32 count;
	u32 max_count;
	u64 dir_ino;
};

/*
 * Initialize RADOS client
 */
int cfs_rados_init(struct cfs_fs_info *fsi);

/*
 * Cleanup RADOS client
 */
void cfs_rados_cleanup(struct cfs_fs_info *fsi);

/*
 * Connect to specified pools
 * Returns 0 on success, negative error on failure
 */
int cfs_connect_pools(struct cfs_fs_info *fsi);

/*
 * Allocate a new inode number atomically
 * Returns positive inode number on success, negative error on failure
 */
int cfs_alloc_inode(struct cfs_fs_info *fsi);

/*
 * Read omap value from meta pool
 * key: omap key to read
 * value: output buffer (allocated by caller)
 * value_len: input: buffer size, output: actual value length
 */
int cfs_omap_read(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		  const char *key, void *value, size_t *value_len);

/*
 * Write omap value to meta pool
 * key: omap key to write
 * value: input buffer
 * value_len: value length
 */
int cfs_omap_write(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		   const char *key, const void *value, size_t value_len);

/*
 * Check if omap key exists
 */
int cfs_omap_exists(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		    const char *key, bool *exists);

/*
 * List omap entries with prefix filter
 */
int cfs_omap_list(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		  const char *start_after, const char *prefix,
		  int max_entries,
		  int (*cb)(const char *key, void *value, size_t len, void *priv),
		  void *priv);

/*
 * Delete omap key
 */
int cfs_omap_delete(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		    const char *key);

/*
 * Create omap entry atomically (fails if exists)
 * Uses CEPH_OSD_OP_ASSERT_VER with ver=0 for creation
 */
int cfs_omap_create_atomic(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
			   const char *key, const void *value, size_t value_len,
			   u64 *ver_out);

/*
 * Conditional update with version check
 * Returns -ERANGE if version mismatch
 */
int cfs_omap_update_cond(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
			 const char *key, const void *value, size_t value_len,
			 u64 expect_ver, u64 *new_ver);

/*
 * Read data from object in data pool
 * offset: object offset
 * length: bytes to read
 * pages: output pages
 */
int cfs_data_read(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		  u64 offset, u64 length, struct page **pages, int num_pages);

/*
 * Write data to object in data pool
 */
int cfs_data_write(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		   u64 offset, u64 length, struct page **pages, int num_pages);

/*
 * Delete object from data pool
 */
int cfs_data_delete(struct cfs_fs_info *fsi, struct ceph_object_id *oid);

/*
 * Truncate object in data pool
 */
int cfs_data_truncate(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		      u64 new_size);

/*
 * Get object stat (size, mtime)
 */
int cfs_data_stat(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		  u64 *size, struct timespec64 *mtime);

/*
 * Check if object exists
 */
int cfs_data_exists(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		    bool *exists);

/*
 * Execute OSD class method (for metadata operations)
 */
int cfs_exec_class_method(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
			  const char *method, const char *class_name,
			  void *in_data, size_t in_len,
			  void *out_data, size_t *out_len);

/*
 * Helper to construct object locator
 */
static inline void cfs_init_oloc(struct ceph_object_locator *oloc,
				  s64 pool_id)
{
	ceph_oloc_init(oloc);
	oloc->pool = pool_id;
}

/*
 * Wait for OSD request completion
 */
int cfs_wait_request(struct ceph_osd_request *req);

/*
 * OSD request completion callback
 */
void cfs_osd_req_callback(struct ceph_osd_request *req);

/*
 * Setup OSD request with pages
 */
int cfs_setup_osd_req_pages(struct ceph_osd_request *req,
			     struct page **pages, int num_pages,
			     bool write);

/*
 * Build vector from pages
 */
void cfs_pages_to_vec(struct page **pages, int num_pages,
		      struct ceph_osd_data *osd_data);

#endif /* _FS_CFS_RADOS_H */
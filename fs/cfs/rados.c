/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CFS - Ceph File System Simple
 *
 * RADOS communication layer implementation
 */

#include <linux/ceph/ceph_client.h>
#include <linux/ceph/ceph_features.h>
#include <linux/ceph/osdmap.h>
#include <linux/ceph/osd_client.h>
#include <linux/ceph/messenger.h>
#include <linux/ceph/rados.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "super.h"
#include "internal.h"
#include "rados.h"

/*
 * Initialize RADOS client
 */
int cfs_rados_init(struct cfs_fs_info *fsi)
{
	struct ceph_client *client;
	struct ceph_options *opts = NULL;
	int ret;

	cfs_debug("rados_init: initializing RADOS client\n");

	if (!fsi->opts->mon_addr) {
		cfs_err("rados_init: no monitor address specified\n");
		return -EINVAL;
	}

	/* Parse monitor addresses */
	opts = ceph_parse_options(fsi->opts->mon_addr, NULL, NULL, NULL, NULL);
	if (IS_ERR(opts)) {
		ret = PTR_ERR(opts);
		cfs_err("rados_init: failed to parse monitor addresses: %d\n", ret);
		return ret;
	}

	/* Create ceph client */
	client = ceph_create_client(opts, fsi);
	if (IS_ERR(client)) {
		ret = PTR_ERR(client);
		cfs_err("rados_init: failed to create ceph client: %d\n", ret);
		ceph_destroy_options(opts);
		return ret;
	}

	/* Connect to cluster */
	ret = ceph_open_session(client);
	if (ret) {
		cfs_err("rados_init: failed to open session: %d\n", ret);
		ceph_destroy_client(client);
		return ret;
	}

	fsi->client = client;
	fsi->osdc = &client->osdc;

	cfs_debug("rados_init: RADOS client initialized successfully\n");
	return 0;
}

/*
 * Cleanup RADOS client
 */
void cfs_rados_cleanup(struct cfs_fs_info *fsi)
{
	if (!fsi)
		return;

	if (fsi->client) {
		ceph_close_session(fsi->client);
		ceph_destroy_client(fsi->client);
		fsi->client = NULL;
		fsi->osdc = NULL;
	}
}

/*
 * Connect to specified pools
 */
int cfs_connect_pools(struct cfs_fs_info *fsi)
{
	struct ceph_client *client = fsi->client;
	s64 pool_id;
	int ret;

	cfs_debug("connect_pools: meta_pool=%s, data_pool=%s\n",
		  fsi->opts->meta_pool, fsi->opts->data_pool);

	/* Look up meta pool */
	pool_id = ceph_pool_name_to_id(client->osdc.osdmap, fsi->opts->meta_pool);
	if (pool_id < 0) {
		cfs_err("connect_pools: meta pool '%s' not found\n",
			fsi->opts->meta_pool);
		return -ENOENT;
	}
	fsi->meta_pool_id = pool_id;

	/* Look up data pool */
	pool_id = ceph_pool_name_to_id(client->osdc.osdmap, fsi->opts->data_pool);
	if (pool_id < 0) {
		cfs_err("connect_pools: data pool '%s' not found\n",
			fsi->opts->data_pool);
		return -ENOENT;
	}
	fsi->data_pool_id = pool_id;

	cfs_debug("connect_pools: meta_pool_id=%lld, data_pool_id=%lld\n",
		  fsi->meta_pool_id, fsi->data_pool_id);

	return 0;
}

/*
 * Wait for OSD request completion
 */
int cfs_wait_request(struct ceph_osd_request *req)
{
	int ret;

	ret = wait_for_completion_timeout(&req->r_completion,
					  CFS_OSD_REQUEST_TIMEOUT);
	if (!ret) {
		cfs_err("wait_request: timeout\n");
		return -ETIMEDOUT;
	}

	ret = req->r_result;
	if (ret < 0) {
		cfs_debug("wait_request: request failed: %d\n", ret);
	}

	return ret;
}

/*
 * OSD request completion callback
 */
static void cfs_osd_req_complete_cb(struct ceph_osd_request *req)
{
	complete(&req->r_completion);
}

/*
 * Allocate OSD request for OMAP operation
 */
static struct ceph_osd_request *cfs_alloc_omap_request(
		struct ceph_osd_client *osdc, s64 pool_id,
		struct ceph_object_id *oid, int num_ops)
{
	struct ceph_osd_request *req;
	struct ceph_object_locator oloc;

	ceph_oloc_init(&oloc);
	oloc.pool = pool_id;

	req = ceph_osdc_alloc_request(osdc, NULL, num_ops, false, GFP_NOFS);
	if (!req)
		return NULL;

	ceph_oid_copy(&req->r_base_oid, oid);
	ceph_oloc_copy(&req->r_base_oloc, &oloc);
	init_completion(&req->r_completion);
	req->r_callback = cfs_osd_req_complete_cb;

	ceph_oloc_destroy(&oloc);
	return req;
}

/*
 * Allocate OSD request for data operation
 */
static struct ceph_osd_request *cfs_alloc_data_request(
		struct ceph_osd_client *osdc, s64 pool_id,
		struct ceph_object_id *oid, int num_ops)
{
	struct ceph_osd_request *req;
	struct ceph_object_locator oloc;

	ceph_oloc_init(&oloc);
	oloc.pool = pool_id;

	req = ceph_osdc_alloc_request(osdc, NULL, num_ops, false, GFP_NOFS);
	if (!req)
		return NULL;

	ceph_oid_copy(&req->r_base_oid, oid);
	ceph_oloc_copy(&req->r_base_oloc, &oloc);
	init_completion(&req->r_completion);
	req->r_callback = cfs_osd_req_complete_cb;

	ceph_oloc_destroy(&oloc);
	return req;
}

/*
 * Setup OSD request with pages
 */
int cfs_setup_osd_req_pages(struct ceph_osd_request *req,
			     struct page **pages, int num_pages, bool write)
{
	struct ceph_osd_data *osd_data;
	enum osd_req_op op_type = write ? CEPH_OSD_OP_WRITE : CEPH_OSD_OP_READ;

	if (write) {
		osd_data = osd_req_op_data(req, 0, extent, osd_data);
		osd_req_data_alloc(req, CEPH_OSD_OP_WRITE, num_pages * PAGE_SIZE);
		ceph_osd_data_pages_init(osd_data, pages, num_pages * PAGE_SIZE, 0, false, false);
	} else {
		osd_data = osd_req_op_data(req, 0, extent, osd_data);
		osd_req_data_alloc(req, CEPH_OSD_OP_READ, num_pages * PAGE_SIZE);
		ceph_osd_data_pages_init(osd_data, pages, num_pages * PAGE_SIZE, 0, false, false);
	}
	/* Suppress unused variable warning */
	(void)op_type;

	return 0;
}

/*
 * Read omap value from meta pool
 */
int cfs_omap_read(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		  const char *key, void *value, size_t *value_len)
{
	struct ceph_osd_client *osdc = fsi->osdc;
	struct ceph_osd_request *req;
	void *out_buf;
	size_t out_len = 0;
	int ret;

	cfs_debug("omap_read: oid=%s, key=%s\n", oid->name, key);

	req = cfs_alloc_omap_request(osdc, fsi->meta_pool_id, oid, 2);
	if (!req)
		return -ENOMEM;

	/* Set operation flags */
	req->r_flags = CEPH_OSD_FLAG_READ;

	/* Add OMAP_GET operation */
	ret = ceph_osdc_call(osdc, oid, &req->r_base_oloc, "omap", "get",
			     CEPH_OSD_FLAG_READ, key, strlen(key), &out_buf, &out_len);
	if (ret) {
		cfs_debug("omap_read: failed: %d\n", ret);
		ceph_osdc_put_request(req);
		return ret;
	}

	/* Copy output to caller */
	if (out_len <= *value_len) {
		memcpy(value, out_buf, out_len);
		*value_len = out_len;
		ret = 0;
	} else {
		*value_len = out_len;
		ret = -ERANGE;
	}

	kfree(out_buf);
	ceph_osdc_put_request(req);
	return ret;
}

/*
 * Write omap value to meta pool
 */
int cfs_omap_write(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		   const char *key, const void *value, size_t value_len)
{
	struct ceph_osd_client *osdc = fsi->osdc;
	struct ceph_osd_request *req;
	int ret;

	cfs_debug("omap_write: oid=%s, key=%s\n", oid->name, key);

	req = cfs_alloc_omap_request(osdc, fsi->meta_pool_id, oid, 2);
	if (!req)
		return -ENOMEM;

	/* Set operation flags */
	req->r_flags = CEPH_OSD_FLAG_WRITE;

	/* Add OMAP_SET operation */
	osd_req_op_omap_set(req, 0, key, value, value_len, -1);

	/* Submit and wait */
	ret = ceph_osdc_start_request(osdc, req, false);
	if (ret) {
		ceph_osdc_put_request(req);
		return ret;
	}

	ret = cfs_wait_request(req);
	ceph_osdc_put_request(req);

	if (ret)
		cfs_debug("omap_write: failed: %d\n", ret);

	return ret;
}

/*
 * Check if omap key exists
 */
int cfs_omap_exists(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		    const char *key, bool *exists)
{
	struct ceph_osd_client *osdc = fsi->osdc;
	struct ceph_osd_request *req;
	__u64 count = 0;
	int ret;

	cfs_debug("omap_exists: oid=%s, key=%s\n", oid->name, key);

	req = cfs_alloc_omap_request(osdc, fsi->meta_pool_id, oid, 2);
	if (!req)
		return -ENOMEM;

	req->r_flags = CEPH_OSD_FLAG_READ;

	/* Use omap get keys to check existence */
	osd_req_op_omap_get_keys(req, 0, key, 1, &count);

	ret = ceph_osdc_start_request(osdc, req, false);
	if (ret) {
		ceph_osdc_put_request(req);
		return ret;
	}

	ret = cfs_wait_request(req);
	if (ret == 0) {
		*exists = (count > 0);
	} else {
		*exists = false;
	}

	ceph_osdc_put_request(req);
	return ret;
}

/*
 * Delete omap key
 */
int cfs_omap_delete(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		    const char *key)
{
	struct ceph_osd_client *osdc = fsi->osdc;
	struct ceph_osd_request *req;
	int ret;

	cfs_debug("omap_delete: oid=%s, key=%s\n", oid->name, key);

	req = cfs_alloc_omap_request(osdc, fsi->meta_pool_id, oid, 2);
	if (!req)
		return -ENOMEM;

	req->r_flags = CEPH_OSD_FLAG_WRITE;

	osd_req_op_omap_rm_keys(req, 0, key, strlen(key));

	ret = ceph_osdc_start_request(osdc, req, false);
	if (ret) {
		ceph_osdc_put_request(req);
		return ret;
	}

	ret = cfs_wait_request(req);
	ceph_osdc_put_request(req);

	return ret;
}

/*
 * Execute OSD class method
 */
int cfs_exec_class_method(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
			  const char *method, const char *class_name,
			  void *in_data, size_t in_len,
			  void *out_data, size_t *out_len)
{
	struct ceph_osd_client *osdc = fsi->osdc;
	struct ceph_object_locator oloc;
	void *response = NULL;
	size_t response_len = 0;
	int ret;

	cfs_debug("exec_class_method: oid=%s, method=%s\n", oid->name, method);

	ceph_oloc_init(&oloc);
	oloc.pool = fsi->meta_pool_id;

	ret = ceph_osdc_call(osdc, oid, &oloc, class_name, method,
			     CEPH_OSD_FLAG_WRITE, in_data, in_len,
			     &response, &response_len);
	if (ret) {
		cfs_debug("exec_class_method: failed: %d\n", ret);
		ceph_oloc_destroy(&oloc);
		return ret;
	}

	if (out_data && out_len) {
		if (response_len <= *out_len) {
			memcpy(out_data, response, response_len);
			*out_len = response_len;
		} else {
			ret = -ERANGE;
		}
	}

	kfree(response);
	ceph_oloc_destroy(&oloc);
	return ret;
}

/*
 * Read data from object in data pool
 */
int cfs_data_read(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		  u64 offset, u64 length, struct page **pages, int num_pages)
{
	struct ceph_osd_client *osdc = fsi->osdc;
	struct ceph_osd_request *req;
	int ret;

	cfs_debug("data_read: oid=%s, offset=%llu, length=%llu\n",
		  oid->name, offset, length);

	req = cfs_alloc_data_request(osdc, fsi->data_pool_id, oid, 2);
	if (!req)
		return -ENOMEM;

	req->r_flags = CEPH_OSD_FLAG_READ;

	/* Add read operation with pages data */
	osd_req_op_extent_init(req, 0, CEPH_OSD_OP_READ, offset, length, 0, 0);
	osd_req_op_extent_osd_data_pages(req, 0, pages, length, 0, false, false);

	ret = ceph_osdc_start_request(osdc, req, false);
	if (ret) {
		ceph_osdc_put_request(req);
		return ret;
	}

	ret = cfs_wait_request(req);
	ceph_osdc_put_request(req);

	if (ret)
		cfs_debug("data_read: failed: %d\n", ret);

	return ret;
}

/*
 * Write data to object in data pool
 */
int cfs_data_write(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		   u64 offset, u64 length, struct page **pages, int num_pages)
{
	struct ceph_osd_client *osdc = fsi->osdc;
	struct ceph_osd_request *req;
	int ret;

	cfs_debug("data_write: oid=%s, offset=%llu, length=%llu\n",
		  oid->name, offset, length);

	req = cfs_alloc_data_request(osdc, fsi->data_pool_id, oid, 2);
	if (!req)
		return -ENOMEM;

	req->r_flags = CEPH_OSD_FLAG_WRITE | CEPH_OSD_FLAG_ONDISK;

	/* Add write operation with pages data */
	osd_req_op_extent_init(req, 0, CEPH_OSD_OP_WRITE, offset, length, 0, 0);
	osd_req_op_extent_osd_data_pages(req, 0, pages, length, 0, false, true);

	ret = ceph_osdc_start_request(osdc, req, false);
	if (ret) {
		ceph_osdc_put_request(req);
		return ret;
	}

	ret = cfs_wait_request(req);
	ceph_osdc_put_request(req);

	if (ret)
		cfs_debug("data_write: failed: %d\n", ret);

	return ret;
}

/*
 * Delete object from data pool
 */
int cfs_data_delete(struct cfs_fs_info *fsi, struct ceph_object_id *oid)
{
	struct ceph_osd_client *osdc = fsi->osdc;
	struct ceph_osd_request *req;
	int ret;

	cfs_debug("data_delete: oid=%s\n", oid->name);

	req = cfs_alloc_data_request(osdc, fsi->data_pool_id, oid, 1);
	if (!req)
		return -ENOMEM;

	req->r_flags = CEPH_OSD_FLAG_WRITE;

	osd_req_op_init(req, 0, CEPH_OSD_OP_DELETE, 0);

	ret = ceph_osdc_start_request(osdc, req, false);
	if (ret) {
		ceph_osdc_put_request(req);
		return ret;
	}

	ret = cfs_wait_request(req);
	ceph_osdc_put_request(req);

	return ret;
}

/*
 * Truncate object in data pool
 */
int cfs_data_truncate(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		      u64 new_size)
{
	struct ceph_osd_client *osdc = fsi->osdc;
	struct ceph_osd_request *req;
	int ret;

	cfs_debug("data_truncate: oid=%s, new_size=%llu\n", oid->name, new_size);

	req = cfs_alloc_data_request(osdc, fsi->data_pool_id, oid, 1);
	if (!req)
		return -ENOMEM;

	req->r_flags = CEPH_OSD_FLAG_WRITE;

	osd_req_op_extent_init(req, 0, CEPH_OSD_OP_TRUNCATE, 0, new_size, 0, 0);

	ret = ceph_osdc_start_request(osdc, req, false);
	if (ret) {
		ceph_osdc_put_request(req);
		return ret;
	}

	ret = cfs_wait_request(req);
	ceph_osdc_put_request(req);

	return ret;
}

/*
 * Initialize inode allocator
 */
int cfs_init_ino_allocator(struct cfs_fs_info *fsi)
{
	spin_lock_init(&fsi->ino_alloc.lock);
	fsi->ino_alloc.local_next = 0;
	fsi->ino_alloc.local_end = 0;
	fsi->ino_alloc.global_version = 0;
	fsi->ino_alloc.initialized = true;

	return 0;
}

/*
 * Allocate inode number with batch reservation
 * Returns: positive inode number on success, negative error code on failure
 */
int cfs_alloc_ino(struct cfs_fs_info *fsi)
{
	struct ceph_object_id oid;
	void *in_data = NULL;
	void *out_data = NULL;
	size_t in_len, out_len;
	struct cfs_allocator_data data;
	u64 new_ino;
	int ret, retries = 0;

	cfs_debug("alloc_ino: starting allocation\n");

retry:
	retries++;
	if (retries > CFS_MAX_RETRIES) {
		cfs_err("alloc_ino: max retries exceeded\n");
		return -EIO;
	}

	/* First try local batch */
	spin_lock(&fsi->ino_alloc.lock);
	if (fsi->ino_alloc.local_next < fsi->ino_alloc.local_end) {
		new_ino = fsi->ino_alloc.local_next++;
		if (new_ino > (u64)INT_MAX) {
			spin_unlock(&fsi->ino_alloc.lock);
			cfs_err("alloc_ino: inode overflow\n");
			return -ERANGE;
		}
		spin_unlock(&fsi->ino_alloc.lock);
		cfs_debug("alloc_ino: allocated from local batch: %llu\n", new_ino);
		return (int)new_ino;
	}
	spin_unlock(&fsi->ino_alloc.lock);

	/* Need to reserve a new batch from RADOS */
	cfs_allocator_oid(&oid);

	/* Read current allocator state */
	out_len = sizeof(data);
	ret = cfs_omap_read(fsi, &oid, CFS_INODE_ALLOCATOR_KEY,
			    &data, &out_len);
	if (ret && ret != -ENOENT) {
		cfs_err("alloc_ino: failed to read allocator: %d\n", ret);
		goto retry;
	}

	if (ret == -ENOENT) {
		/* First time initialization */
		data.next_ino = cpu_to_le64(CFS_MIN_USER_INO);
		data.version = cpu_to_le64(1);
	} else {
		/* Parse current state */
		u64 current_next = le64_to_cpu(data.next_ino);
		u64 current_version = le64_to_cpu(data.version);

		/* Update to reserve batch */
		data.next_ino = cpu_to_le64(current_next + fsi->opts->ino_batch_size);
		data.version = cpu_to_le64(current_version + 1);
	}

	/* Write back atomically with version check */
	in_data = &data;
	in_len = sizeof(data);
	ret = cfs_omap_write(fsi, &oid, CFS_INODE_ALLOCATOR_KEY,
			     in_data, in_len);
	if (ret) {
		cfs_debug("alloc_ino: write failed, retrying: %d\n", ret);
		goto retry;
	}

	/* Update local state */
	spin_lock(&fsi->ino_alloc.lock);
	fsi->ino_alloc.local_next = le64_to_cpu(data.next_ino) - fsi->opts->ino_batch_size;
	fsi->ino_alloc.local_end = le64_to_cpu(data.next_ino);
	fsi->ino_alloc.global_version = le64_to_cpu(data.version);
	new_ino = fsi->ino_alloc.local_next++;
	if (new_ino > (u64)INT_MAX) {
		spin_unlock(&fsi->ino_alloc.lock);
		cfs_err("alloc_ino: inode overflow\n");
		return -ERANGE;
	}
	spin_unlock(&fsi->ino_alloc.lock);

	cfs_debug("alloc_ino: allocated new batch, ino=%llu\n", new_ino);
	return (int)new_ino;
}

/*
 * List omap entries with callback
 */
int cfs_omap_list(struct cfs_fs_info *fsi, struct ceph_object_id *oid,
		  const char *start_after, const char *prefix,
		  int max_entries,
		  int (*cb)(const char *key, void *value, size_t len, void *priv),
		  void *priv)
{
	struct ceph_osd_client *osdc = fsi->osdc;
	struct ceph_osd_request *req;
	struct ceph_osd_data *osd_data;
	void *response = NULL;
	size_t response_len = 0;
	char *key, *value;
	size_t key_len, value_len;
	u32 num_entries = 0;
	u32 offset = 0;
	int ret, cb_ret;

	cfs_debug("omap_list: oid=%s, start_after=%s, prefix=%s\n",
		  oid->name, start_after ? start_after : "(null)",
		  prefix ? prefix : "(null)");

	/* Build omap list request */
	req = cfs_alloc_omap_request(osdc, fsi->meta_pool_id, oid, 3);
	if (!req)
		return -ENOMEM;

	req->r_flags = CEPH_OSD_FLAG_READ;

	/* Add omap get keys and values operation */
	osd_req_op_omap_get_keys_vals(req, 0, start_after ? start_after : "",
				      start_after ? strlen(start_after) : 0,
				      max_entries);

	ret = ceph_osdc_start_request(osdc, req, false);
	if (ret) {
		ceph_osdc_put_request(req);
		return ret;
	}

	ret = cfs_wait_request(req);
	if (ret) {
		ceph_osdc_put_request(req);
		return ret;
	}

	/* Parse response - this requires iterating over returned omap data */
	/* The response format is: num_entries, then for each entry: key_len, key, value_len, value */
	/* Note: This is a simplified implementation - actual OSD omap response parsing
	 * would require proper buffer parsing based on OSD response structure */

	/* For now, use the request's response buffer */
	osd_data = osd_req_op_data(req, 0, omap, osd_data);
	if (osd_data && osd_data->pages) {
		void *buf = page_address(osd_data->pages[0]);
		size_t buf_len = osd_data->length;

		/* Parse omap header */
		if (buf_len >= sizeof(u32)) {
			num_entries = le32_to_cpu(*((__le32 *)buf));
			offset = sizeof(u32);
		}

		/* Iterate over entries */
		while (num_entries > 0 && offset < buf_len) {
			/* Read key length */
			if (offset + sizeof(u32) > buf_len)
				break;
			key_len = le32_to_cpu(*((__le32 *)(buf + offset)));
			offset += sizeof(u32);

			/* Read key */
			if (offset + key_len > buf_len)
				break;
			key = buf + offset;
			offset += key_len;

			/* Read value length */
			if (offset + sizeof(u32) > buf_len)
				break;
			value_len = le32_to_cpu(*((__le32 *)(buf + offset)));
			offset += sizeof(u32);

			/* Read value */
			if (offset + value_len > buf_len)
				break;
			value = buf + offset;
			offset += value_len;

			/* Skip if prefix doesn't match */
			if (prefix && strncmp(key, prefix, strlen(prefix)) != 0)
				continue;

			/* Call callback */
			cb_ret = cb(key, value, value_len, priv);
			if (cb_ret)
				break;  /* Stop iteration */

			num_entries--;
		}
	}

	ceph_osdc_put_request(req);
	return 0;
}
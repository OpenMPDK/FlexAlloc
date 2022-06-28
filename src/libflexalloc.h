/**
 * flexalloc end-user API.
 *
 * Copyright (C) 2021 Joel Granados <j.granados@samsung.com>
 * Copyright (C) 2021 Jesper Devantier <j.devantier@samsung.com>
 * Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>
 *
 * @file libflexalloc.h
 */
#ifndef __LIBFLEXALLOC_H_
#define __LIBFLEXALLOC_H_
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "flexalloc_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

struct flexalloc;

/**
 * Open flexalloc system
 * @param opts pointer to initialized fla open opts struct
 * @param flexalloc pointer to flexalloc system handle, if successful, otherwise uninitialized.
 *
 * @return On success 0 and *fs pointing to a flexalloc system handle. On error, non-zero
 * and *fs being uninitialized.
 */
int
fla_open(struct fla_open_opts *opts, struct flexalloc **fs);

/**
 * Close flexalloc system.
 *
 * Flush meta data changes to disk and close flexalloc system for further access.
 *
 * @param fs flexalloc system handle
 *
 * @return On success 0 with *fs being freed and the device released.
 */
int
fla_close(struct flexalloc *fs);

/**
 * Flush flexalloc system state to disk.
 *
 * Flush meta data changes to disk.
 *
 * @param fs flexalloc system handle
 *
 * @return On success 0.
 */
int
fla_sync(struct flexalloc *fs);

/**
 * Create pool.
 *
 * Creates and allocates a new pool in memory using the given name as its
 * identifier. New pool is not created if it already exists with the same
 * object size. Objects from the pool will span `obj_nlb` logical blocks.
 * Will Not flush changes.
 *
 * NOTE: to acquire the handle for an existing pool, use fla_pool_open()
 *
 * @param fs flexalloc system handle
 * @param name Name of pool
 * @param name_len Length of name string
 * @param obj_nlb Size of objects to allocate from pool, in number of logical blocks.
 * @param pool Pool handle, is initialized if acquisition of pool went well.
 *
 * @return On success, 0 and *handle is initialized. On error, non-zero and *handle
 * is undefined.
 */
int
fla_pool_create(struct flexalloc *fs, const char *name, int name_len, uint32_t obj_nlb,
                struct fla_pool **pool);

/**
 * Destroy pool and its resources.
 *
 * Releases the pool and any slabs, and thereby objects, which it
 * manages from memroy. Will not flush changes.
 *
 * @param fs flexalloc system handle
 * @param pool Pool handle
 *
 * @return On success, 0, the pool has been released. On error, non-zero -
 * the pool has not been removed.
 */

int
fla_pool_destroy(struct flexalloc *fs, struct fla_pool * pool);

/**
 * Allocates a pool handle
 *
 * Open and allocate (malloc) pool handle of existing pool, if it exists.
 * Note, the pool must have been created earlier using fla_pool_create()
 *
 * @param fs flexalloc system handle
 * @param name name of pool to find
 * @param pool pool handle to initialize.
 *
 * @return On success, 0 and handle is initialized. On error, non-zero with
 * handle is undefined.
 */
int
fla_pool_open(struct flexalloc *fs, const char *name, struct fla_pool **pool);

/**
 * Closes the pool handle by freeing its memory.
 *
 * It will not close any of the related slabs or objects
 *
 * @param fs flexalloc system handle
 * @param pool Pool handle to free
 */
void
fla_pool_close(struct flexalloc *fs, struct fla_pool * pool);

/**
 * @brief return a free object related to a pool handle
 *
 * Acquires a slab if the pool is missing one.
 * Will adjust the pool empty, full and partial slab lists accordingly
 *
 * @param fs flexalloc system handle
 * @param pool Pool that will contain object
 * @param object Object handle to initialize
 * @return 0 on success. non zero otherwise
 */
int
fla_object_create(struct flexalloc * fs, struct fla_pool * pool,
                  struct fla_object * object);

/**
 * @brief Makes sure that object handle is valid
 *
 * Loads slab cache in case it has not been already loaded
 *
 * @param fs flexalloc system handle
 * @param pool Pool that contains object
 * @param object Object to check
 * @return 0 on success. non zero otherwise.
 */
int
fla_object_open(struct flexalloc * fs, struct fla_pool * pool,
                struct fla_object * object);

/**
 * @brief Makes an object available to the pool again
 *
 * Will not release an acquired slab by the pool
 *
 * @param fs flexalloc system handle
 * @param pool Pool containing object
 * @param object Pointer to object to be released
 * @return Zero on success. non zero otherwise
 */
int
fla_object_destroy(struct flexalloc *fs, struct fla_pool * pool,
                   struct fla_object * object);

/**
 *
 * @brief Close a flexalloc object
 *
 * Runs logic signifying that the object will not be actively used.
 * Might be a noop.
 *
 * @param fs flexalloc system handle
 * @param pool_handle flexalloc pool handle associated with obj
 * @param obj flexalloc object handle to close
 * @return 0 on success. non zero otherwise.
 */
int
fla_object_close(struct flexalloc *fs, struct fla_pool const *pool_handle, struct fla_object *obj);

/**
 * @brief Allocate an aligned (to underlying file system) buffer
 *
 * @param fs flexalloc system handle
 * @param nbytes Number of bytes to allocate
 */
void *
fla_buf_alloc(struct flexalloc const *fs, size_t nbytes);

/**
 * @brief Free allocated memory by fla_buff_alloc
 *
 * @param fs flexalloc system handle
 * @param buf Buffer to free
 */
void
fla_buf_free(struct flexalloc const * fs, void *buf);

/**
 * @brief Read len bytes into buf
 *
 * @param fs flexalloc system handle
 * @param pool Handle to the pool containing the obj
 * @param object Read from this object
 * @param buf Read into this buffer
 * @param offset Number of bytes from beginning of object where the read begins.
 * @param len Number of bytes to read
 * @return Zero on success. non zero otherwise
 */
int
fla_object_read(struct flexalloc const * fs, struct fla_pool const * pool,
                struct fla_object const * object, void * buf, size_t offset, size_t len);

/**
 * @brief Write len bytes from buf
 *
 * @param fs flexalloc system handle
 * @param pool Handle to the pool containing the obj
 * @param object Write to this object
 * @param buf Write from this buffer
 * @param offset Number of bytes from the beginning of the object where the write begins
 * @param len Number of bytes to write
 * @return Zero on success. non zero otherwise
 */
int
fla_object_write(struct flexalloc * fs, struct fla_pool const * pool,
                 struct fla_object const * object, void const * buf, size_t offset, size_t len);

/**
 * @brief Same as fla_object_write but offset and len can be unaligned values
 *
 * @param fs flexalloc system handle
 * @param pool Handle to the pool containing the obj
 * @param object Write from this object
 * @param buf Write from this buffer
 * @param offset Number of bytes from beginning of object
 * @param len Number of bytes to write
 * @return Zero on success. non zero otherwise
 */
int
fla_object_unaligned_write(struct flexalloc * fs,
                           struct fla_pool const * pool,
                           struct fla_object const * object, void const * buf, size_t offset,
                           size_t len);

/**
 * @brief Associate object with pool
 *
 * @param fs flexalloc system handle
 * @param pool flexalloc pool handle to associate obj with
 * @param object flexalloc object handle to associate with pool
 * @param force Force pool root to be updated to obj even if it is already set
 * @return Zero on success, non zero otherwise
 */
int
fla_pool_set_root_object(struct flexalloc const * const fs,
                         struct fla_pool const * pool,
                         struct fla_object const *object, fla_root_object_set_action act);

/**
 * @brief Get root object associated with pool
 *
 * @param fs flexalloc system handle
 * @param pool flexalloc pool handle to associate obj with
 * @param object flexalloc object handle associated with pool on success
 * @return Zero on success, non zero otherwise
 */
int
fla_pool_get_root_object(struct flexalloc const * const fs,
                         struct fla_pool const * pool,
                         struct fla_object *object);
/**
 * @brief Return size of objects in terms of nlb with the pool
 *
 * @param fs flexalloc system handle
 * @param pool_handle flexalloc pool handle to associate obj with
 * @return Zero on failure, obj nlb for the pool otherwise
 */
uint32_t
fla_pool_obj_nlb(struct flexalloc const *const fs, struct fla_pool const *pool_handle);

/**
 * @brief Return the number of sectors in a zone device zone
 *
 * @param fs flexalloc system handle
 * @return number of sectors in each zone
 */
uint64_t
fla_fs_nzsect(struct flexalloc const * const fs);

bool
fla_fs_zns(struct flexalloc const *const fs);

/**
 *
 * @brief Indicate that a pool should hand out striped objects
 *
 * @param fs flexalloc system handle
 * @param pool_handle flexalloc pool handle to set as being striped
 * @param strp_nobjs number of objects to stripe across
 * @param strp_nbytes size of each stripe chunk in bytes
 * @return int indicating if pool striping parameters were applied
 */
int
fla_pool_set_strp(struct flexalloc *fs, struct fla_pool *pool, uint32_t strp_nobjs,
                  uint32_t strp_nbytes);

#ifdef __cplusplus
}
#endif

#endif // __LIBFLEXALLOC_H_

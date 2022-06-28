#ifndef FLEXALLOC_SHARED_H_
#define FLEXALLOC_SHARED_H_
#include <stdint.h>
#include <libxnvme.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLA_ERR_ERROR 1001
#define FLA_ERR_ALL_SLABS_USED 2001

struct flexalloc;

struct fla_pool;
struct flexalloc;

/// flexalloc open options
///
/// Minimally the dev_uri needs to be set
/// If the md_dev is set than flexalloc md will be stored on this device
/// The xnvme open options are optionally set at open time as well
struct fla_open_opts
{
  const char *dev_uri;
  const char *md_dev_uri;
  struct xnvme_opts *opts;
};

/// flexalloc object handle
///
/// The object handle is created from the slab id and the index of the object entry
/// within the slab.
struct fla_object
{
  /// ID of the parent slab (0..nslab-1)
  uint32_t slab_id;
  /// offset of object within the slab
  uint32_t entry_ndx;
};


struct fla_pool
{
  /// h2 hash of entry.
  /// Can be used to check that acquired pool entry's name matches the name of
  /// the pool when the handle was made. This may prevent following stale
  /// handles to repurposed pool entries.
  uint64_t h2;
  /// offset of entry in the pool entries table
  uint32_t ndx;
};

typedef enum
{
  ROOT_OBJ_SET_DEF = 0,
  ROOT_OBJ_SET_FORCE = 1 << 0,
  ROOT_OBJ_SET_CLEAR = 1 << 1
} fla_root_object_set_action;

/**
 * @brief Return the number of bytes in a block in fs
 *
 * @param fs flexalloc system handl
 * @return number of bytes in a block
 */
int32_t
fla_fs_lb_nbytes(struct flexalloc const * const fs);

struct fla_fns
{
  int (*close)(struct flexalloc *fs);
  int (*sync)(struct flexalloc *fs);
  int (*pool_open)(struct flexalloc *fs, const char *name, struct fla_pool **pool);
  void (*pool_close)(struct flexalloc *fs, struct fla_pool *pool);
  int (*pool_create)(struct flexalloc *fs, const char *name, int name_len, uint32_t obj_nlb,
                     struct fla_pool **pool);
  int (*pool_destroy)(struct flexalloc *fs, struct fla_pool *pool);
  int (*pool_set_strp)(struct flexalloc *fs, struct fla_pool *pool, uint32_t strp_nobjs,
                       uint32_t strp_nbytes);
  int (*object_open)(struct flexalloc *fs, struct fla_pool *pool, struct fla_object *object);
  int (*object_create)(struct flexalloc *fs, struct fla_pool *pool, struct fla_object *object);
  int (*object_destroy)(struct flexalloc *fs, struct fla_pool *pool, struct fla_object *object);
  int (*pool_set_root_object)(struct flexalloc const * const fs, struct fla_pool const * pool,
                              struct fla_object const *object, fla_root_object_set_action act);
  int (*pool_get_root_object)(struct flexalloc const * const fs, struct fla_pool const * pool,
                              struct fla_object *object);
  int (*fla_action)();
};

struct flexalloc *
fla_fs_alloc();

void
fla_fs_free(struct flexalloc *fs);

int
fla_fs_set_user(void *user_data);

void *
fla_fs_get_user();

#ifdef __cplusplus
}
#endif

#endif // FLEXALLOC_SHARED_H_

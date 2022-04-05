// Copyright (C) 2021 Joel Granados <j.granados@samsung.com>
// Copyright (C) 2021 Jesper Devantier <j.devantier@samsung.com>

#include "libflexalloc.h"
#include "flexalloc_xnvme_env.h"
#include "flexalloc.h"

int
fla_close(struct flexalloc *fs)
{
  return fs->fns.close(fs);
}

int
fla_sync(struct flexalloc *fs)
{
  return fs->fns.sync(fs);
}

int
fla_pool_create(struct flexalloc *fs, const char *name, int name_len, uint32_t obj_nlb,
                struct fla_pool **pool)
{
  return fs->fns.pool_create(fs, name, name_len, obj_nlb, pool);
}

int
fla_pool_destroy(struct flexalloc *fs, struct fla_pool * pool)
{
  return fs->fns.pool_destroy(fs, pool);
}

int
fla_pool_open(struct flexalloc *fs, const char *name, struct fla_pool **pool)
{
  return fs->fns.pool_open(fs, name, pool);
}

void
fla_pool_close(struct flexalloc *fs, struct fla_pool * pool)
{
  fs->fns.pool_close(fs, pool);
}

int
fla_object_create(struct flexalloc * fs, struct fla_pool * pool,
                  struct fla_object * object)
{
  return fs->fns.object_create(fs, pool, object);
}

int
fla_object_open(struct flexalloc * fs, struct fla_pool * pool,
                struct fla_object * object)
{
  return fs->fns.object_open(fs, pool, object);
}

int
fla_object_destroy(struct flexalloc *fs, struct fla_pool * pool,
                   struct fla_object * object)
{
  return fs->fns.object_destroy(fs, pool, object);
}

void *
fla_buf_alloc(struct flexalloc const *fs, size_t nbytes)
{
  return fla_xne_alloc_buf(fs->dev.dev, nbytes);
}

void
fla_buf_free(struct flexalloc const * fs, void *buf)
{
  fla_xne_free_buf(fs->dev.dev, buf);
}

int
fla_pool_set_root_object(struct flexalloc const * const fs,
                         struct fla_pool const * pool,
                         struct fla_object const *object, fla_root_object_set_action act)
{
  return fs->fns.pool_set_root_object(fs, pool, object, act);
}

int
fla_pool_get_root_object(struct flexalloc const * const fs,
                         struct fla_pool const * pool,
                         struct fla_object *object)
{
  return fs->fns.pool_get_root_object(fs, pool, object);
}


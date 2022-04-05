/**
 * flexalloc disk structures.
 *
 * Copyright (C) 2022 Adam Manzanares <a.manzanares@samsung.com>
 *
 * @file flexalloc_znd.h
 */
#ifndef __FLEXALLOC_ZND_H_
#define __FLEXALLOC_ZND_H_
#include "src/flexalloc.h"

int
fla_znd_manage_zones_object_finish(struct flexalloc *fs, struct fla_pool const *pool_handle,
                                   struct fla_object *obj);

int
fla_znd_manage_zones_object_reset(struct flexalloc *fs, struct fla_pool const *pool_handle,
                                  struct fla_object * obj);
#endif // __FLEXALLOC_ZND_H

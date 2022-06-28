// Copyright (C) 2022 Adam Manzanares <a.manzanares@samsung.com>
#include "flexalloc_znd.h"
#include "flexalloc_xnvme_env.h"
#include "flexalloc_mm.h"
#include "flexalloc_util.h"

int
fla_znd_manage_zones_object_finish(struct flexalloc *fs, struct fla_pool const *pool_handle,
                                   struct fla_object *obj)
{
  int err = 0;
  uint64_t obj_slba = fla_object_slba(fs, obj, pool_handle);
  struct fla_pool_entry *pool_entry = &fs->pools.entries[pool_handle->ndx];

  for (uint32_t strp = 0; strp < pool_entry->strp_nobjs; strp++)
  {
    err = fla_xne_dev_znd_send_mgmt(fs->dev.dev, obj_slba + (fs->geo.nzsect * strp),
                                    XNVME_SPEC_ZND_CMD_MGMT_SEND_FINISH, false);
    if (FLA_ERR(err, "fla_xne_dev_znd_send_mgmt_finish()"))
      return err;
  }
  return err;
}

int
fla_znd_manage_zones_object_reset(struct flexalloc *fs, struct fla_pool const *pool_handle,
                                  struct fla_object * obj)
{
  int err = 0;
  uint64_t obj_slba = fla_object_slba(fs, obj, pool_handle);
  struct fla_pool_entry *pool_entry = &fs->pools.entries[pool_handle->ndx];

  for (uint32_t strp = 0; strp < pool_entry->strp_nobjs; strp++)
  {

    err = fla_xne_dev_znd_send_mgmt(fs->dev.dev, obj_slba + (fs->geo.nzsect * strp),
                                    XNVME_SPEC_ZND_CMD_MGMT_SEND_RESET, false);
    if (FLA_ERR(err, "fla_xne_dev_znd_send_mgmt_reset()"))
      return err;
  }
  return err;
}


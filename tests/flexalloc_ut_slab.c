// Copyright (C) 2021 Joel Granados <j.granados@samsung.com>
#include "tests/flexalloc_tests_common.h"
#include "flexalloc_util.h"
#include "flexalloc_mm.h"
#include "flexalloc_ll.h"
#include <stdint.h>

struct test_vals
{
  uint32_t npools;
  uint32_t min_disk_lbs; // Will be overridden by real size on "real" HW.
  float slab_size_p; // slab size in percent of disk size
  float obj_size_p; // obje size in percent of slab size
};

static int test_slabs(struct test_vals * test_vals);
static int test_check_slab_pointers(struct flexalloc * fs, const uint32_t expected_size);

int
main(int argc, char ** argv)
{
  int err = 0;

  struct test_vals test_vals [] =
  {
    {.npools = 1, .min_disk_lbs = 100, .slab_size_p = 0.8, .obj_size_p = 0.8 }
    , {.npools = 1, .min_disk_lbs = 100, .slab_size_p = 0.8, .obj_size_p = 0.2 }
    , {.npools = 2, .min_disk_lbs = 100, .slab_size_p = 0.4, .obj_size_p = 0.8 }
    , {.npools = 2, .min_disk_lbs = 100, .slab_size_p = 0.4, .obj_size_p = 0.2 }
    , {.npools = 4, .min_disk_lbs = 100, .slab_size_p = 0.2, .obj_size_p = 0.8 }
    , {.npools = 4, .min_disk_lbs = 100, .slab_size_p = 0.2, .obj_size_p = 0.2 }
    , {.npools = 0, .min_disk_lbs = 0, .slab_size_p = 0, .obj_size_p = 0}
  };

  for(int i = 0 ; true ; ++i)
  {
    if (test_vals[i].npools == 0)
      goto exit;
    err = test_slabs(&test_vals[i]);
    if(FLA_ERR(err, "test_slabs()"))
    {
      goto exit;
    };
  }

exit:
  return err;
}

int
test_slabs(struct test_vals * test_vals)
{
  int err = 0, ret;
  struct fla_ut_dev dev;
  struct flexalloc *fs;
  struct fla_slab_header *slab_header, *slab_error;
  uint32_t slab_nlb, obj_nlb, init_free_slabs;

  err = fla_ut_dev_init(test_vals->min_disk_lbs, &dev);
  if (FLA_ERR(err, "fla_ut_dev_init()"))
  {
    goto exit;
  }
  test_vals->min_disk_lbs = dev.nblocks;

  /* Skip for ZNS.
   * If we are testing ZNS, we will automatically modify slab size
   * rendering all our tests useless.
   */
  if(dev._is_zns)
    goto exit;

  slab_error = malloc(sizeof(struct fla_slab_header));
  if (FLA_ERR(!slab_error, "malloc()"))
  {
    err = -ENOMEM;
    goto exit;
  }

  slab_nlb = (uint32_t)(test_vals->min_disk_lbs * test_vals->slab_size_p);
  obj_nlb = (uint32_t)(slab_nlb * test_vals->obj_size_p);

  err = fla_ut_fs_create(slab_nlb, test_vals->npools, &dev, &fs);
  if (FLA_ERR(err, "fla_ut_fs_create()"))
  {
    goto free_slab_error;
  }

  init_free_slabs = *fs->slabs.fslab_num;

  FLA_ASSERTF(test_vals->min_disk_lbs > fla_geo_slabs_lb_off(&fs->geo),
              "Slabs start after disk has ended (%"PRIu64" > %"PRIu64"",
              test_vals->min_disk_lbs, fla_geo_slabs_lb_off(&fs->geo));

  /* we need at least one slab */
  err |= FLA_ASSERTF(init_free_slabs >= 1, "Unexpected number of free slabs (%d >= 1)",
                     init_free_slabs);

  err |= FLA_ASSERTF(*fs->slabs.fslab_head == 0,
                     "Unexpected head ID (%d == %d)", *fs->slabs.fslab_head, 0);

  /* Acquire all the slabs and then release them all */
  for(uint32_t slab_offset = 0 ; slab_offset < init_free_slabs ; ++slab_offset)
  {
    slab_header = (void*)fs->slabs.headers + (slab_offset * sizeof(struct fla_slab_header));

    err = fla_acquire_slab(fs, obj_nlb, &slab_header);
    if(FLA_ERR(err, "fla_acquire_slab()"))
    {
      goto close_fs;
    }

    err = FLA_ASSERT(slab_header->next == FLA_LINKED_LIST_NULL,
                     "Next pointer is not null after slab format");
    err |= FLA_ASSERT(slab_header->prev == FLA_LINKED_LIST_NULL,
                      "Prev pointer is not null after slab format");
    if(FLA_ERR(err, "FLA_ASSERT()"))
    {
      goto close_fs;
    }

    const uint32_t curr_free_slabs = init_free_slabs - (slab_offset + 1);
    err = FLA_ASSERTF(*fs->slabs.fslab_num >= curr_free_slabs,
                      "Unexpected number of free slabs (%d >= %d)",
                      *fs->slabs.fslab_num, curr_free_slabs);
    if(FLA_ERR(err, "FLA_ASSERTF()"))
    {
      goto close_fs;
    }

    err = test_check_slab_pointers(fs, curr_free_slabs);
    if(FLA_ERR(err, "test_check_slab_pointers()"))
    {
      goto close_fs;
    }
  }

  /* If we acquire another slab, we should receive an error */
  ret = fla_acquire_slab(fs, obj_nlb, &slab_error);
  err = FLA_ASSERT(ret != 0, "Acquire of an empty free list did NOT fail");
  if(FLA_ERR(err, "FLA_ASSERT()"))
  {
    goto close_fs;
  }

  for(uint32_t slab_offset = 0 ; slab_offset < init_free_slabs ; ++slab_offset)
  {
    slab_header = (void*)fs->slabs.headers + (slab_offset * sizeof(struct fla_slab_header));

    err = fla_release_slab(fs, slab_header);
    if(FLA_ERR(err, "fla_release_slab()"))
      goto close_fs;

    err = FLA_ASSERTF(*fs->slabs.fslab_num >= slab_offset + 1,
                      "Unexpected number of free slabs (%d >= %d)",
                      *fs->slabs.fslab_num, slab_offset + 1);
    if(FLA_ERR(err, "FLA_ASSERTF()"))
      goto close_fs;
  }

close_fs:
  ret = fla_ut_fs_teardown(fs);
  if (FLA_ERR(ret, "fla_ut_fs_teardown()"))
  {
    err = ret;
  }

free_slab_error:
  free(slab_error);

exit:
  return err;
}

int
test_check_slab_pointers(struct flexalloc * fs, const uint32_t curr_free_slabs)
{
  int err = 0;
  struct fla_slab_header * curr_slab;
  uint32_t curr_slab_id, size_from_head = 0;

  /* check next pointers */
  curr_slab_id = *fs->slabs.fslab_head;
  for (uint32_t i = 0 ; i <= curr_free_slabs && curr_slab_id != INT32_MAX; ++i)
  {
    curr_slab = fla_slab_header_ptr(curr_slab_id, fs);
    if((err = -FLA_ERR(!curr_slab, "fla_slab_header_ptr()")))
    {
      goto exit;
    }
    curr_slab_id = curr_slab->next;
    size_from_head++;
  }

  err = FLA_ASSERTF(size_from_head >= curr_free_slabs,
                    "Unexpected size when starting from head (%d == %d)",
                    size_from_head, curr_free_slabs);

  /* check prev pointers */
  curr_slab_id = *fs->slabs.fslab_tail;
  for (uint32_t i = 0; i <= curr_free_slabs && curr_slab_id != INT32_MAX; ++i)
  {
    curr_slab = fla_slab_header_ptr(curr_slab_id, fs);
    if((err = -FLA_ERR(!curr_slab, "fla_slab_header_ptr()")))
    {
      goto exit;
    }
    curr_slab_id = curr_slab->prev;
    size_from_head++;
  }

exit:
  return err;
}

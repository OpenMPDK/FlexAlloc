// Copyright (C) 2021 Joel Granados <j.granados@samsung.com>
#include "tests/flexalloc_tests_common.h"
#include "flexalloc_util.h"
#include "flexalloc_mm.h"
#include "flexalloc_ll.h"
#include <stdint.h>

struct test_vals
{
  uint32_t npools;
  uint32_t slab_nlb;
  uint32_t disk_min_lbs;
  uint32_t obj_nlb;
};

static int test_slabs(struct test_vals * test_vals);
static int test_check_slab_pointers(struct flexalloc * fs, const uint32_t expected_size);

#define FLA_UT_SLAB_NUMBER_OF_TESTS 4

int
main(int argc, char ** argv)
{
  int err = 0;
  bool fla_test_set = is_globalenv_set("FLA_TEST_DEV");
  struct test_vals test_vals [FLA_UT_SLAB_NUMBER_OF_TESTS] =
  {
    {.npools = 2, .slab_nlb = 2, .disk_min_lbs = fla_test_set?0:9, .obj_nlb = 1 }
    , {.npools = 2, .slab_nlb = 2, .disk_min_lbs = fla_test_set?0:21, .obj_nlb = 1 }
    , {.npools = 2, .slab_nlb = 20, .disk_min_lbs = fla_test_set?0:50, .obj_nlb = 2 }
    , {.npools = 2, .slab_nlb = 5, .disk_min_lbs = fla_test_set?0:18, .obj_nlb = 1 }
  };

  for(int i = 0 ; i < FLA_UT_SLAB_NUMBER_OF_TESTS ; ++i)
  {
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
  uint64_t available_lb_for_slabs;
  uint32_t expected_slabs;

  err = fla_ut_dev_init(test_vals->disk_min_lbs, &dev);
  if (FLA_ERR(err, "fla_ut_dev_init()"))
  {
    goto exit;
  }

  /* Skip for ZNS.
   * If we are testing ZNS, we will automatically modify slab size
   * rendering all our tests useless.
   */
  if(dev._is_zns)
    goto exit;

  if(test_vals->disk_min_lbs == 0)
    test_vals->disk_min_lbs = dev.nblocks;

  slab_error = malloc(sizeof(struct fla_slab_header));
  if (FLA_ERR(!slab_error, "malloc()"))
  {
    err = -ENOMEM;
    goto exit;
  }

  err = fla_ut_fs_create(test_vals->slab_nlb, test_vals->npools, &dev, &fs);
  if (FLA_ERR(err, "fla_ut_fs_create()"))
  {
    goto free_slab_error;
  }

  FLA_ASSERTF(test_vals->disk_min_lbs > fla_geo_slabs_lb_off(&fs->geo),
              "Slabs start after disk has ended (%"PRIu64" > %"PRIu64"",
              test_vals->disk_min_lbs, fla_geo_slabs_lb_off(&fs->geo));
  available_lb_for_slabs = test_vals->disk_min_lbs - fla_geo_slabs_lb_off(&fs->geo);
  expected_slabs = available_lb_for_slabs / test_vals->slab_nlb;

  /* Test values in struct fla_slabs */
  err |= FLA_ASSERTF(*fs->slabs.fslab_num == expected_slabs,
                     "Unexpected number of free slabs (%d == %d)",
                     *fs->slabs.fslab_num, expected_slabs);

  /*err |= FLA_ASSERTF(*fs->slabs.fslab_head == 0,
                     "Unexpected head ID (%d == %d)", *fs->slabs.fslab_head, 0);*/

  err |= FLA_ASSERTF(*fs->slabs.fslab_tail == expected_slabs - 1,
                     "Unexpected tail ID (%d == %d)",
                     *fs->slabs.fslab_tail, expected_slabs - 1);

  /* Acquire all the slabs and then release them all */
  for(uint32_t slab_offset = 0 ; slab_offset < expected_slabs ; ++slab_offset)
  {
    slab_header = (void*)fs->slabs.headers + (slab_offset * sizeof(struct fla_slab_header));

    err = fla_acquire_slab(fs, test_vals->obj_nlb, &slab_header);
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

    const uint32_t free_slabs = expected_slabs - (slab_offset + 1);
    err = FLA_ASSERTF(*fs->slabs.fslab_num == free_slabs,
                      "Unexpected number of free slabs (%d == %d)",
                      *fs->slabs.fslab_num, free_slabs);
    if(FLA_ERR(err, "FLA_ASSERTF()"))
    {
      goto close_fs;
    }

    err = test_check_slab_pointers(fs, free_slabs);
    if(FLA_ERR(err, "test_check_slab_pointers()"))
    {
      goto close_fs;
    }
  }

  /* If we acquire another slab, we should receive an error */
  ret = fla_acquire_slab(fs, test_vals->obj_nlb, &slab_error);
  err = FLA_ASSERT(ret != 0, "Acquire of an empty free list did NOT fail");
  if(FLA_ERR(err, "FLA_ASSERT()"))
  {
    goto close_fs;
  }

  for(uint32_t slab_offset = 0 ; slab_offset < expected_slabs ; ++slab_offset)
  {
    slab_header = (void*)fs->slabs.headers + (slab_offset * sizeof(struct fla_slab_header));

    err = fla_release_slab(fs, slab_header);
    if(FLA_ERR(err, "fla_release_slab()"))
      goto close_fs;

    err = FLA_ASSERTF(*fs->slabs.fslab_num == slab_offset + 1,
                      "Unexpected number of free slabs (%d == %d)",
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
test_check_slab_pointers(struct flexalloc * fs, const uint32_t expected_size)
{
  int err = 0;
  struct fla_slab_header * curr_slab;
  uint32_t curr_slab_id, size_from_head = 0;

  /* check next pointers */
  curr_slab_id = *fs->slabs.fslab_head;
  for (uint32_t i = 0 ; i <= expected_size && curr_slab_id != INT32_MAX; ++i)
  {
    curr_slab = fla_slab_header_ptr(curr_slab_id, fs);
    if((err = -FLA_ERR(!curr_slab, "fla_slab_header_ptr()")))
    {
      goto exit;
    }
    curr_slab_id = curr_slab->next;
    size_from_head++;
  }

  err = FLA_ASSERTF(size_from_head == expected_size,
                    "Unexpected size when starting from head (%d == %d)",
                    size_from_head, expected_size);

  /* check prev pointers */
  curr_slab_id = *fs->slabs.fslab_tail;
  for (uint32_t i = 0; i <= expected_size && curr_slab_id != INT32_MAX; ++i)
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "src/libflexalloc.h"
#include "src/flexalloc.h"
#include "src/flexalloc_util.h"
#include "tests/flexalloc_tests_common.h"

struct test_vals
{
  uint64_t blk_num;
  uint32_t npools;
  uint32_t slab_nlb;
  uint32_t obj_nlb;
  uint32_t strp_nobj;
  uint32_t strp_nlbs;
  uint32_t xfer_snlb;
  uint32_t xfer_nlbs;
};

#define NUM_TESTS 3
struct test_vals tests[] =
{
  // Simple write all.
  {
    .blk_num = 40000, .slab_nlb = 4000, .npools = 1, .obj_nlb = 2,
    .strp_nobj = 2, .strp_nlbs = 1, .xfer_snlb = 0, .xfer_nlbs = 4
  },

  // start from second chunk and wrap around
  {
    .blk_num = 40000, .slab_nlb = 4000, .npools = 1, .obj_nlb = 16,
    .strp_nobj = 4, .strp_nlbs = 4, .xfer_snlb = 4, .xfer_nlbs = 16
  },

  // several transfers in each object
  {
    .blk_num = 40000, .slab_nlb = 4000, .npools = 1, .obj_nlb = 16,
    .strp_nobj = 4, .strp_nlbs = 4, .xfer_snlb = 4, .xfer_nlbs = 48
  },
};

int
test_strp(struct test_vals test_vals)
{
  int err = 0, ret;
  char * pool_handle_name, *write_buf, *read_buf;
  size_t buf_len;
  uint64_t xfer_offset;
  struct fla_ut_dev dev;
  struct flexalloc *fs = NULL;
  struct fla_pool *pool_handle;
  struct fla_object obj;
  struct fla_open_opts open_opts = {0};

  pool_handle_name = "mypool";

  err = fla_ut_dev_init(test_vals.blk_num, &dev);
  if (FLA_ERR(err, "fla_ut_dev_init()"))
    goto exit;

  if (dev._is_zns)
  {
    // why *2? -> To run these tests we need at least one striped object. The
    // slab must have enough space to fit the striped object (nsect_zn *
    // strp_nobj) and the metadata. We can calculate the meatadata and grow it
    // by that size or take out the big hammer and just double the size. I
    // chose the latter.
    test_vals.slab_nlb = dev.nsect_zn * test_vals.strp_nobj * 2;
    test_vals.obj_nlb = dev.nsect_zn;
  }

  err = fla_ut_fs_create(test_vals.slab_nlb, test_vals.npools, &dev, &fs);
  if (FLA_ERR(err, "fla_ut_fs_create()"))
  {
    goto teardown_ut_dev;
  }

  buf_len = test_vals.xfer_nlbs * dev.lb_nbytes;
  xfer_offset = test_vals.xfer_snlb * dev.lb_nbytes;

  err = fla_pool_create(fs, pool_handle_name, strlen(pool_handle_name), test_vals.obj_nlb,
                        &pool_handle);
  if(FLA_ERR(err, "fla_pool_create()"))
    goto teardown_ut_fs;

  err = fla_pool_set_strp(fs, pool_handle, test_vals.strp_nobj,
                          dev.lb_nbytes * test_vals.strp_nlbs);
  if (FLA_ERR(err, "fla_pool_set_strp()"))
    goto release_pool;

  err = fla_object_create(fs, pool_handle, &obj);
  if(FLA_ERR(err, "fla_object_create()"))
    goto release_pool;

  write_buf = fla_buf_alloc(fs, buf_len);
  if((err = FLA_ERR(!write_buf, "fla_buf_alloc()")))
    goto release_object;

  fla_t_fill_buf_random(write_buf, buf_len);
  write_buf[buf_len] = '\0';

  err = fla_object_write(fs, pool_handle, &obj, write_buf, xfer_offset, buf_len);
  if(FLA_ERR(err, "fla_object_write()"))
    goto free_write_buffer;

  err = fla_close(fs);
  if(FLA_ERR(err, "fla_close()"))
    goto free_write_buffer;

  open_opts.dev_uri = dev._dev_uri;
  open_opts.md_dev_uri = dev._md_dev_uri;
  err = fla_open(&open_opts, &fs);
  if(FLA_ERR(err, "fla_open()"))
    goto free_write_buffer;

  fla_pool_close(fs, pool_handle);
  err = fla_pool_open(fs, pool_handle_name, &pool_handle);
  if(FLA_ERR(err, "fla_pool_open()"))
    goto free_write_buffer;

  err = fla_object_open(fs, pool_handle, &obj);
  if(FLA_ERR(err, "fla_object_open()"))
    goto free_write_buffer;

  read_buf = fla_buf_alloc(fs, buf_len);
  if((err = FLA_ERR(!read_buf, "fla_buf_alloc()")))
    goto free_write_buffer;
  memset(read_buf, 0, buf_len);
  read_buf[buf_len] = '\0';

  err = fla_object_read(fs, pool_handle, &obj, read_buf, xfer_offset, buf_len);
  if(FLA_ERR(err, "fla_obj_read()"))
    goto free_read_buffer;

  /*
   * Compare that the string (and terminating NULL) was read back
   *
   * If we are on a ZNS drive and xfer_snlb > 0 the write should fail
   * and the write_buf shall be different than read_buf
   */
  err = memcmp(write_buf,  read_buf, buf_len + 1);
  if(dev._is_zns)
  {
    if(test_vals.xfer_snlb > 0)
      err = !(err != 0);
  }

  if(FLA_ERR(err, "Unexpected value for memcmp"))
    goto free_write_buffer;

  // Free the object, which should reset a zone
  ret = fla_object_destroy(fs, pool_handle, &obj);
  if(FLA_ERR(ret, "fla_object_destroy()"))
    err = ret;

  // Allocate a new object
  err = fla_object_create(fs, pool_handle, &obj);
  if(FLA_ERR(err, "fla_object_create()"))
    goto release_pool;

  // Will fail on zns without object reset
  err = fla_object_write(fs, pool_handle, &obj, write_buf, 0, buf_len);
  if(FLA_ERR(err, "fla_object_write()"))
    goto free_write_buffer;

free_read_buffer:
  fla_buf_free(fs, read_buf);

free_write_buffer:
  fla_buf_free(fs, write_buf);

release_object:
  ret = fla_object_destroy(fs, pool_handle, &obj);
  if(FLA_ERR(ret, "fla_object_destroy()"))
    err = ret;

release_pool:
  ret = fla_pool_destroy(fs, pool_handle);
  if(FLA_ERR(ret, "fla_pool_destroy()"))
    err = ret;

teardown_ut_fs:
  ret = fla_ut_fs_teardown(fs);
  if (FLA_ERR(ret, "fla_ut_fs_teardown()"))
  {
    err = ret;
  }

teardown_ut_dev:
  ret = fla_ut_dev_teardown(&dev);
  if (FLA_ERR(ret, "fla_ut_dev_teardown()"))
  {
    err = ret;
  }

exit:
  return err;
}

int
main(int argc, char **argv)
{
  int err = 0;
  // We seed the rand with pid so it produces different values always
  srand(getpid());

  for (int i = 0 ; i < NUM_TESTS ; ++i)
    err |= test_strp(tests[i]);

  return err;
}

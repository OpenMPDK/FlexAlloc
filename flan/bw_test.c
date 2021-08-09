// Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>

#include <libflexalloc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <libxnvme.h>

#include "flan.h"

#define POOL_NAME "TEST"
#define USAGE "./test OBJ_SZ IO_SZ NUM_OBJS NUM_STRPS TO_ACCESS"
#define OPT_IO_SIZE 2097152

int main(int argc, char **argv)
{
  struct flan_handle *flanh;
  uint64_t oh;
  int ret = 0;
  char *dev_uri = getenv("FLAN_TEST_DEV"), *obj_name = NULL;
  char *md_dev_uri = getenv("FLAN_TEST_MD_DEV");
  uint64_t obj_sz, io_sz, num_objs, count, num_strps, to_access;
  char *endofarg;
  struct xnvme_timer time;

  if (!dev_uri)
  {
    printf("Set env var FLAN_TEST_DEV in order to run test\n");
    return 0;
  }

  if (argc != 6)
  {
    printf("Object size not supplied, Usasge:\n%s\n", USAGE);
    return 0;
  }

  errno = 0;
  obj_sz = strtoul(argv[1], &endofarg, 0);
  if (endofarg == argv[1])
  {
    printf("Number not found in OBJ_SZ argument\n");
    return 0;
  }

  if (errno)
  {
    perror("strotoul obj_sz");
    return 0;
  }

  errno = 0;
  io_sz = strtoul(argv[2], &endofarg, 0);
  if (endofarg == argv[2])
  {
    printf("Number not found in IO_SZ argument\n");
    return 0;
  }

  if (errno)
  {
    perror("strotoul");
    return 0;
  }

  errno = 0;
  num_objs = strtoul(argv[3], &endofarg, 0);
  if (endofarg == argv[3])
  {
    printf("Number not found in NUM_OBJS argument\n");
    return 0;
  }

  if (errno)
  {
    perror("strotoul");
    return 0;
  }

  errno = 0;
  num_strps = strtoul(argv[4], &endofarg, 0);
  if (endofarg == argv[4])
  {
    printf("Number not found in NUM_OBJS argument\n");
    return 0;
  }

  if (errno)
  {
    perror("strotoul");
    return 0;
  }

  to_access = strtoul(argv[5], &endofarg, 0);
  if (endofarg == argv[4])
  {
    printf("Number not found in NUM_OBJS argument\n");
    return 0;
  }

  if (errno)
  {
    perror("strotoul");
    return 0;
  }

  // Open flexalloc device, create pool, create named object and close
  printf("Opening flan, and creating \"test\" objects\n");
  if (md_dev_uri)
    ret = flan_init(dev_uri, md_dev_uri, POOL_NAME, obj_sz, &flanh);
  else
    ret = flan_init(dev_uri, NULL, POOL_NAME, obj_sz, &flanh);

  if (ret)
    goto out;

  ret = flan_pool_set_strp(flanh, num_strps, OPT_IO_SIZE / num_strps);
  if (ret)
    goto out;

  obj_name = flan_buf_alloc(obj_sz, flanh);
  if (!obj_name)
    goto out_close;

  for(count = 0; count < num_objs; count++)
  {
    snprintf(obj_name, FLAN_OBJ_NAME_LEN_MAX, "%s.%lu", POOL_NAME, count);
    ret = flan_object_open(obj_name, flanh, &oh, FLAN_OPEN_FLAG_CREATE | FLAN_OPEN_FLAG_WRITE);
    if (ret)
    {
      break;
    }

    if (count && !(count % 1024))
		printf("Created:%lu objects\n", count);

    uint64_t to_write = to_access;
    xnvme_timer_start(&time);
    while (to_write)
    {
      uint64_t cur_io_sz = io_sz;
      if (cur_io_sz > to_write)
        cur_io_sz = to_write;

      ret = flan_object_write(oh, obj_name, to_access - to_write, cur_io_sz, flanh);
      if (ret)
      {
        printf("Write error at obj:%lu offset:%lu\n", count, to_access - to_write);
        break;
      }

      to_write -= cur_io_sz;
    }

    xnvme_timer_stop(&time);
    printf("Object %lu\n Write BW", count);
    xnvme_timer_bw_pr(&time, "wall-clock", to_access);
    flan_object_close(oh, flanh);
  }

  printf("Created %lu objects\n", count);
  printf("Listing all objects in the pool\n");
  for (count = 0; count < num_objs; count++)
  {
    snprintf(obj_name, FLAN_OBJ_NAME_LEN_MAX, "%s.%lu", POOL_NAME, count);
    if (count && !(count % 1024))
      printf("Objects found so far:%lu\n", count);

    ret = flan_object_open(obj_name, flanh, &oh, FLAN_OPEN_FLAG_READ);
    if (ret)
      break;

    uint64_t to_read = to_access;
    xnvme_timer_start(&time);
    while (to_read)
    {
      uint64_t cur_io_sz = io_sz;
      if (cur_io_sz > to_read)
        cur_io_sz = to_read;

      ret = flan_object_read(oh, obj_name, to_access - to_read, cur_io_sz, flanh);
      if (ret != cur_io_sz)
      {
        printf("Read error at obj:%lu offset:%lu\n", count, to_access - to_read);
        break;
      }

      to_read -= cur_io_sz;
    }

    xnvme_timer_stop(&time);
    printf("Object %lu\n Read BW", count);
    xnvme_timer_bw_pr(&time, "wall-clock", to_access);
    flan_object_close(oh, flanh);
  }

  printf("\nFound %lu objects in the pool\n", count);
  printf("Closing flan and flexalloc\n");
  flan_close(flanh);
  printf("Reopening flan\n");
  if (md_dev_uri)
    ret = flan_init(dev_uri, md_dev_uri, POOL_NAME, obj_sz, &flanh);
  else
    ret = flan_init(dev_uri, NULL, POOL_NAME, obj_sz, &flanh);

  if (ret)
    goto out;
  printf("Listing all objects in the pool\n");
  for (count = 0; count < num_objs; count++)
  {
    snprintf(obj_name, FLAN_OBJ_NAME_LEN_MAX, "%s.%lu", POOL_NAME, count);
    if (count && !(count % 1024))
      printf("Objects found so far:%lu\n", count);

    ret = flan_object_open(obj_name, flanh, &oh, FLAN_OPEN_FLAG_READ);
    if (ret)
      break;

    uint64_t to_read = to_access;
    xnvme_timer_start(&time);
    while (to_read)
    {
      uint64_t cur_io_sz = io_sz;
      if (cur_io_sz > to_read)
        cur_io_sz = to_read;

      ret = flan_object_read(oh, obj_name, to_access - to_read, cur_io_sz, flanh);
      if (ret != cur_io_sz)
      {
        printf("Read error at obj:%lu offset:%lu\n", count, to_access - to_read);
        break;
      }

      to_read -= cur_io_sz;
    }

    xnvme_timer_stop(&time);
    printf("Object %lu\n Read BW", count);
    xnvme_timer_bw_pr(&time, "wall-clock", to_access);
    flan_object_close(oh, flanh);
  }

  printf("\n");
  printf("\nFound %lu objects in the pool\n", count);

out_close:
  flan_close(flanh);

out:
  if (obj_name)
    free(obj_name);

  return ret;
}

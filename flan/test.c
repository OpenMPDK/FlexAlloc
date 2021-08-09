// Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>

#include <libflexalloc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "flan.h"

#define POOL_NAME "TEST"
#define USAGE "./test OBJ_SZ"

int main(int argc, char **argv)
{
  struct flan_handle *flanh;
  uint64_t oh;
  int ret = 0, count = 0, rdcount = 0;
  char *dev_uri = getenv("FLAN_TEST_DEV"), *obj_name = NULL;
  char *md_dev_uri = getenv("FLAN_TEST_MD_DEV");
  unsigned int obj_sz;
  unsigned int mid;
  char *endofarg;

  if (!dev_uri)
  {
    printf("Set env var FLAN_TEST_DEV in order to run test\n");
    return 0;
  }

  if (argc != 2)
  {
    printf("Object size not supplied, Usasge:\n%s\n", USAGE);
    return 0;
  }

  errno = 0;
  obj_sz = strtoul(argv[1], &endofarg, 0);
  if (endofarg == argv[1])
  {
    printf("Number not found in argument\n");
    return 0;
  }

  if (errno)
  {
    perror("strotoul");
    return 0;
  }

  mid = obj_sz / 2;
  // Open flexalloc device, create pool, create named object and close
  printf("Opening flan, and creating \"test\" objects\n");
  if (md_dev_uri)
    ret = flan_init(dev_uri, md_dev_uri, POOL_NAME, obj_sz, &flanh);
  else
    ret = flan_init(dev_uri, NULL, POOL_NAME, obj_sz, &flanh);

  if (ret)
    goto out;

  obj_name = flan_buf_alloc(obj_sz, flanh);
  if (!obj_name)
    goto out_close;

  do
  {
    snprintf(obj_name, FLAN_OBJ_NAME_LEN_MAX, "%s.%d", POOL_NAME, count);
    ret = flan_object_open(obj_name, flanh, &oh, FLAN_OPEN_FLAG_CREATE | FLAN_OPEN_FLAG_WRITE);
    if (ret)
    {
      break;
    }

    count++;
	printf("Created:%d objects\n", count);

    ret = flan_object_write(oh, obj_name, 0, mid - 1, flanh);
    ret = flan_object_read(oh, obj_name, 0, mid - 1, flanh);
    ret = flan_object_write(oh, obj_name + mid - 1, mid - 1, mid + 1, flanh);
    flan_object_close(oh, flanh);
  } while (!ret);

  printf("Created %d objects\n", count);
  printf("Listing all objects in the pool\n");
  for (rdcount = 0; rdcount < count; rdcount++)
  {
    snprintf(obj_name, FLAN_OBJ_NAME_LEN_MAX, "%s.%d", POOL_NAME, rdcount);
    if (!(rdcount % 1024))
      printf("Objects found so far:%d\n",rdcount);

    ret = flan_object_open(obj_name, flanh, &oh, FLAN_OPEN_FLAG_READ);
    if (ret)
      break;

    ret = flan_object_read(oh, obj_name, 0, mid - 1, flanh);
    ret = flan_object_read(oh, obj_name, mid - 1, mid + 1, flanh);
    if (ret != mid + 1)
      break;

    flan_object_close(oh, flanh);
  }

  printf("\nFound %d objects in the pool\n", rdcount);
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
  for (rdcount = 0; rdcount < count; rdcount++)
  {
    snprintf(obj_name, FLAN_OBJ_NAME_LEN_MAX, "%s.%d", POOL_NAME, rdcount);
    if (!(rdcount % 1024))
      printf("Objects found so far:%d\n", rdcount);

    ret = flan_object_open(obj_name, flanh, &oh, FLAN_OPEN_FLAG_READ);
    if (ret)
      break;

    ret = flan_object_read(oh, obj_name, 0, obj_sz, flanh);
    if (ret != obj_sz)
      break;

    flan_object_close(oh, flanh);
  }

  printf("\n");
  printf("\nFound %d objects in the pool\n", rdcount);

out_close:
  flan_close(flanh);

out:
  if (obj_name)
    free(obj_name);

  return ret;
}

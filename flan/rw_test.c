// Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>

#include <libflexalloc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "flan.h"

#define POOL_NAME "TEST"
#define OBJ_NAME "TEST_OBJ"
#define USAGE "./test OBJ_SZ"

size_t ap_pos;

int flan_buf_verify(char *obj_buf, unsigned int len)
{
  for (int i = 0; i < len; i++)
  {
    if (obj_buf[i] != (char)i)
    {
      printf("Mismatch at offset:%u\n", i);
      return 1;
    }
  }

  return 0;
}

int verify(uint64_t oh, char *buf, unsigned int obj_sz, struct flan_handle *flanh)
{
  int ret;

  memset(buf, 0, obj_sz);
  ret = flan_object_read(oh, buf, 0, ap_pos, flanh);
  if (ret != ap_pos)
  {
    printf("Error reading %luB from start of object\n", ap_pos);
    goto out;
  }

  ret = flan_buf_verify(buf, ap_pos);
  if (ret)
    printf("Error verifying first %luB\n", ap_pos);

out:
  return ret;
}

int append_and_verify(uint64_t oh, char *buf, size_t len, unsigned int obj_sz,
                      struct flan_handle *flanh)
{
  int ret;

  for (size_t i = 0; i < len; i++)
    buf[i + ap_pos] = (char)(i+ap_pos);

  ret = flan_object_write(oh, buf + ap_pos, ap_pos, len, flanh);
  if (ret)
  {
    printf("Error writing:%luB off:%lu\n", len, ap_pos);
    goto out;
  }

  ap_pos += len;
  ret = verify(oh, buf, obj_sz, flanh);

out:
  return ret;
}

int main(int argc, char **argv)
{
  struct flan_handle *flanh;
  uint64_t oh;
  int ret = 0;
  char *dev_uri = getenv("FLAN_TEST_DEV");
  char *md_dev_uri = getenv("FLAN_TEST_MD_DEV");
  unsigned int obj_sz;
  char *endofarg, *obj_buf = NULL;

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

  // Open flexalloc device, create pool, create named object and close
  printf("Opening flan, and creating \"test\" objects\n");
  if (md_dev_uri)
    ret = flan_init(dev_uri, md_dev_uri, POOL_NAME, obj_sz, &flanh);
  else
    ret = flan_init(dev_uri, NULL, POOL_NAME, obj_sz, &flanh);

  if (ret)
    goto out;

  obj_buf = flan_buf_alloc(obj_sz, flanh);
  if (!obj_buf)
    goto out_close;

  ret = flan_object_open(OBJ_NAME, flanh, &oh, FLAN_OPEN_FLAG_CREATE | FLAN_OPEN_FLAG_WRITE);
  if (ret)
  {
    printf("Error opening object:%s\n", OBJ_NAME);
    goto out_close;
  }

  ap_pos = 0;
  if (append_and_verify(oh, obj_buf, 2048, obj_sz, flanh)) // AL START, UNAL END, Single Block
    goto out_close;

  if (append_and_verify(oh, obj_buf, 4096, obj_sz, flanh)) // UNAL START, UNAL END, Spans two blocks
    goto out_close;

  if (append_and_verify(oh, obj_buf, 2048, obj_sz, flanh)) // UNAL START, AL END, Single Block
    goto out_close;

  if (append_and_verify(oh, obj_buf, 9192, obj_sz, flanh)) // AL START, UNAL END, Multi Block
    goto out_close;

  if (append_and_verify(oh, obj_buf, 8192, obj_sz, flanh)) // UNAL START, UNAL END, Multi Block
    goto out_close;

  if (append_and_verify(oh, obj_buf, 11288, obj_sz, flanh)) // UNAL START, AL END, Multi Block
    goto out_close;

  if (append_and_verify(oh, obj_buf, 4096, obj_sz, flanh)) // AL START, AL END, Single Block
    goto out_close;

  if (append_and_verify(oh, obj_buf, 4096, obj_sz, flanh)) // AL START, AL END, Single Block
    goto out_close;

  if (append_and_verify(oh, obj_buf, 6, obj_sz, flanh)) // AL START, UNAL END, Single Block
    goto out_close;

  if (append_and_verify(oh, obj_buf, 12276, obj_sz, flanh)) // UNAL START, UNAL END, Spans multiple blocks
    goto out_close;

  printf("Total data written:%luB\n", ap_pos);
  flan_object_close(oh, flanh);
  printf("About to flan close\n");
  flan_close(flanh);
  // Re open flan
  printf("Re opening flan\n");
  if (md_dev_uri)
    ret = flan_init(dev_uri, md_dev_uri, POOL_NAME, obj_sz, &flanh);
  else
    ret = flan_init(dev_uri, NULL, POOL_NAME, obj_sz, &flanh);

  if (ret)
    goto out;

  ret = flan_object_open(OBJ_NAME, flanh, &oh, FLAN_OPEN_FLAG_READ);
  if (ret)
  {
    printf("Error opening object:%s\n", OBJ_NAME);
    goto out_close;
  }

  ret = verify(oh, obj_buf, obj_sz, flanh);

out_close:
  printf("About to flan close\n");
  flan_close(flanh);
  free(obj_buf);

out:
  return ret;
}

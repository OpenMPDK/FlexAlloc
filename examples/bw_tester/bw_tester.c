// Copyright (C) 2022 Adam Manzanares <a.manzanares@samsung.com>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <libflexalloc.h>
#include <libxnvme.h>
#include <unistd.h>

#define POOL_NAME "TEST_POOL"
#define USAGE "./bw_tester fla_dev fla_md_dev num_rw obj_size_blks strp_objs strp_nbytes wrt_nbytes verify num_strp_objs"

int r_w_obj(struct flexalloc *fs, struct fla_pool *pool, struct fla_object *obj,
            uint64_t num, uint64_t wrt_nbytes, bool write, bool verify, int rand_num)
{
  int ret;
  char *buf;
  struct xnvme_timer time;

  buf = fla_buf_alloc(fs, wrt_nbytes);
  if (!buf) {
    printf("Allocating object buffer fails\n");
    return -ENOMEM;
  }

  if (write)
  {
    for (uint64_t cur_byte = 0; cur_byte < wrt_nbytes; cur_byte++)
      buf[cur_byte] = (char)cur_byte + rand_num;
  }

  xnvme_timer_start(&time);
  for (uint64_t cur = 0; cur < num; cur++) {
    ret = write
      ? fla_object_write(fs, pool, obj, buf, cur * wrt_nbytes, wrt_nbytes)
      : fla_object_read(fs, pool, obj, buf, cur * wrt_nbytes, wrt_nbytes);

    if (ret) {
      printf("Object %s fails. Cur:%lu\n", write ? "write" : "read", cur);
      return ret;
    }

    if (!write && verify)
    {
      for (uint64_t cur_byte = 0; cur_byte < wrt_nbytes; cur_byte++)
      {
        if (buf[cur_byte] != (char)(cur_byte + rand_num))
        {
          printf("Data mismatch cur:%lu offset:%lu,expected:[%c],got:[%c]\n", cur, cur_byte,
                 (char)cur_byte, buf[cur_byte]);
          return -1;
        }
      }
    }
  }

  xnvme_timer_stop(&time);

  if (write)
    xnvme_timer_bw_pr(&time, "wall-clock write", wrt_nbytes * num);
  else
    xnvme_timer_bw_pr(&time, "wall-clock read", wrt_nbytes * num);

  return ret;
}

int main(int argc, char **argv)
{
  struct flexalloc *fs;
  struct fla_pool *pool;
  struct fla_object obj;
  int ret;
  char *dev, *md_dev;
  uint64_t num_writes, obj_nlb, strp_nobjs, strp_nbytes, wrt_nbytes, num_strp_objs;
  bool verify;
  struct fla_open_opts open_opts = {0};
  struct xnvme_opts x_opts = xnvme_opts_default();

  if (argc != 10) {
    printf("Usage:%s\n", USAGE);
    return -1;
  }

  dev = argv[1];
  md_dev = argv[2];
  num_writes = atoi(argv[3]);
  obj_nlb = atoi(argv[4]);
  strp_nobjs = atoi(argv[5]);
  strp_nbytes = atoi(argv[6]);
  wrt_nbytes = atoi(argv[7]);
  verify = atoi(argv[8]);
  num_strp_objs = atoi(argv[9]);

  if (num_strp_objs == 0) // Fill until failure when num_strp_objs == 0
    num_strp_objs = num_strp_objs - 1;

  open_opts.dev_uri = dev;
  open_opts.md_dev_uri = md_dev;
  open_opts.opts = &x_opts;
  open_opts.opts->async = "io_uring_cmd";
  ret = fla_open(&open_opts, &fs);
  if (ret) {
    printf("Error on open\n");
    goto exit;
  }

  ret = fla_pool_create(fs, POOL_NAME, strlen(POOL_NAME), obj_nlb, &pool);
  if (ret) {
    printf("Error on pool create\n");
    goto close;
  }

  ret = fla_pool_set_strp(fs, pool, strp_nobjs, strp_nbytes);
  if (ret) {
    printf("Error setting pool strp sz\n");
    goto close;
  }

  srand(getpid());
  for(int i = 0 ; i < num_strp_objs ; ++i)
  {
    int rand_num = rand();
    ret = fla_object_create(fs, pool, &obj);
    if (ret) {
      printf("Object create fails\n");
      goto close;
    }

    ret = r_w_obj(fs, pool, &obj, num_writes, wrt_nbytes, true, false, rand_num);
    if(ret)
      goto close;

    ret = r_w_obj(fs, pool, &obj, num_writes, wrt_nbytes, false, verify, rand_num);
    if(ret)
      goto close;

    ret = fla_object_close(fs , pool, &obj);
    if(ret)
      goto close;
  }

close:
  fla_close(fs);

exit:
  return ret;
}

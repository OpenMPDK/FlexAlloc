// Copyright (C) 2021 Joel Granados <j.granados@samsung.com>
// Copyright (C) 2021 Jesper Devantier <j.devantier@samsung.com>
// Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>

#define _XOPEN_SOURCE 500
#include <libxnvme_geo.h>
#include "tests/flexalloc_tests_common.h"
#include "flexalloc_xnvme_env.h"
#include "flexalloc_util.h"
#include "libflexalloc.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/loop.h>
#include <strings.h>

int
fla_ut_temp_file_create(const int size, char * created_name)
{
  int err = 0;
  int tmp_dev_fd = 0;
  char * remove_bfile_env = NULL;

  int ret = snprintf(created_name, 25, "fla_device_file_XXXXXX");
  if((err = -FLA_ERR(ret < 0, "snprintf()")))
  {
    goto exit;
  }

  tmp_dev_fd = mkstemp(created_name);
  if((err = FLA_ERR_ERRNO(tmp_dev_fd<0, "mkostemp()")))
  {
    goto exit;
  }

  char * buf = malloc(size);
  if(FLA_ERR(!buf, "malloc()"))
  {
    err = -ENOMEM;
    goto exit;
  }

  bzero(buf, size);

  err = write(tmp_dev_fd, buf, size) < 0;
  if((err = FLA_ERR_ERRNO(err, "write()")))
  {
    goto free_buf_error;
  }

  remove_bfile_env = getenv(FLA_TEST_LPBK_REMOVE_BFILE);
  if(!remove_bfile_env || *remove_bfile_env == '1')
  {
    // unlink the file so it removes file when unused
    err = unlink(created_name);
    if((err = FLA_ERR_ERRNO(err, "unlink()")))
    {
      goto free_buf_error;
    }
  }

  //exit:
  free(buf);
  return tmp_dev_fd;

free_buf_error:
  free(buf);

exit:
  return err;
}

// allocate and setup loopback device
int
fla_ut_lpbk_dev_alloc(uint64_t block_size, uint64_t nblocks,
                      struct fla_ut_lpbk **loop)
{
  int err = 0, loop_ctrl_fd = 0, devnr = -1, sprint_ret;

  (*loop) = malloc(sizeof(struct fla_ut_lpbk));
  if(FLA_ERR(!(*loop), "malloc()"))
  {
    err = -ENOMEM;
    goto exit;
  }

  (*loop)->block_size = block_size;
  (*loop)->size = block_size * nblocks;

  (*loop)->dev_name = (char*)malloc(FLA_UT_DEV_NAME_SIZE);
  if(FLA_ERR(!((*loop)->dev_name), "malloc()"))
  {
    err = -ENOMEM;
    goto free_loop;
  }

  (*loop)->bfile_name = (char*)malloc(FLA_UT_BACKING_FILE_NAME_SIZE);
  if(FLA_ERR(!(*loop)->bfile_name, "malloc()"))
  {
    err = -ENOMEM;
    goto free_dev_name;
  }

  // create loop back
  loop_ctrl_fd = open("/dev/loop-control", O_RDWR);
  if((err = -FLA_ERR_ERRNO(loop_ctrl_fd == -1, "open() /dev/loop-control")))
  {
    goto free_bfile_name;
  }

  // lock /dev/loop-control block until released, all the file.
  err = lockf(loop_ctrl_fd, F_LOCK, 0);
  if((err = FLA_ERR_ERRNO(err, "lockf()")))
  {
    goto close_loop_ctrl;
  }

  // Free loop number file descriptor?
  devnr = ioctl(loop_ctrl_fd, LOOP_CTL_GET_FREE);
  if((err = FLA_ERR_ERRNO(devnr==-1, "ioctl() LOOP_CTL_GET_FREE")))
  {
    goto close_loop_ctrl;
  }

  sprint_ret = sprintf((*loop)->dev_name, "/dev/loop%d", devnr);
  if((err = FLA_ERR_ERRNO(sprint_ret<0, "sprintf()")))
  {
    goto close_loop_ctrl;
  }

  (*loop)->dev_fd = -1;
  (*loop)->dev_fd = open((*loop)->dev_name, O_RDWR);
  if((err = FLA_ERR_ERRNO((*loop)->dev_fd == -1, "open()")))
  {
    goto close_loop_ctrl;
  }

  // create temp file
  (*loop)->bfile_fd = fla_ut_temp_file_create((*loop)->size, (*loop)->bfile_name);
  if((err = FLA_ERR((*loop)->bfile_fd < 0,"fla_ut_temp_file_create()")))
  {
    goto close_loop_fd;
  }

  err = ioctl((*loop)->dev_fd, LOOP_SET_FD, (*loop)->bfile_fd);
  if((err = FLA_ERR_ERRNO(err == -1, "ioctl() LOOP_SET_FD")))
  {
    goto close_backing_file;
  }

  err = ioctl((*loop)->dev_fd, LOOP_SET_BLOCK_SIZE, (*loop)->block_size);
  if((err = FLA_ERR_ERRNO(err, "ioctl() LOOP_SET_BLOCK_SIZE")))
  {
    goto teardown_loop;
  }

  // closing the loop_ctrl file releases the locks!
  err = close(loop_ctrl_fd);
  if((err = FLA_ERR_ERRNO(err, "close()")))
  {
    goto teardown_loop;
  }

  return err;

teardown_loop:
  err = ioctl((*loop)->dev_fd, LOOP_CLR_FD);
  err = FLA_ERR_ERRNO(err==-1, "ioctl()");

close_backing_file:
  err = close((*loop)->bfile_fd);
  err = FLA_ERR_ERRNO(err, "close()");

close_loop_fd:
  err = close((*loop)->dev_fd);
  err = FLA_ERR_ERRNO(err, "close()");

close_loop_ctrl:
  // This will also release the lock!
  err = close(loop_ctrl_fd);
  err = FLA_ERR_ERRNO(err, "close()");

free_bfile_name:
  free((*loop)->bfile_name);

free_dev_name:
  free ((*loop)->dev_name);

free_loop:
  free ((*loop));

exit:
  return err;
}

// free loopback device and associated resources
int
fla_ut_lpbk_dev_free(struct fla_ut_lpbk *loop)
{
  int err = 0;
  if (!loop)
    return 0;

  err = ioctl(loop->dev_fd, LOOP_CLR_FD);
  if((err = FLA_ERR_ERRNO(err==-1, "ioctl()")))
  {
    goto exit;
  }

  err = close(loop->bfile_fd);
  if((err = FLA_ERR_ERRNO(err, "close()")))
  {
    goto exit;
  }

  err = close(loop->dev_fd);
  if((err = FLA_ERR_ERRNO(err, "close()")))
  {
    goto exit;
  }

  free(loop->bfile_name);
  free(loop->dev_name);
  free(loop);

exit:
  return err;
}

int
fla_ut_lpbk_overwrite(const char c, struct fla_ut_lpbk * lpbk)
{
  int err = 0;
  const size_t bsz = 4096;
  char buf[bsz];
  off_t pos;
  ssize_t written;

  err = lseek(lpbk->bfile_fd, 0, SEEK_SET) < 0;
  if((err = FLA_ERR_ERRNO(err, "lseek()")))
  {
    goto exit;
  }

  memset(buf, c, bsz);
  for (pos = 0; pos < lpbk->size; pos += written)
  {
    written = write(lpbk->bfile_fd, buf, fla_min(lpbk->size - pos, bsz));
    if((err = FLA_ERR_ERRNO(written < 0, "write()")))
    {
      goto exit;
    }

    if (written == 0)
      break;
  }

  err = fsync(lpbk->bfile_fd);
  FLA_ERR_ERRNO(err, "fsync()");

exit:
  return err;
}

int
fla_ut_lpbk_offs_blk_fill(struct fla_ut_lpbk * lpbk)
{
  int err = 0;
  uint64_t * buf = NULL;

  if((err = FLA_ERR(lpbk->block_size <= 0, "Invalid args")))
  {
    goto exit;
  }
  buf = malloc(lpbk->block_size);
  if(FLA_ERR(!buf, "malloc()"))
  {
    err = -ENOMEM;
    goto exit;
  }

  err = lseek(lpbk->bfile_fd, 0, SEEK_SET) < 0;
  if((err = FLA_ERR_ERRNO(err, "lseek()")))
  {
    goto free_buf;
  }

  for (uint64_t off = 0; off < lpbk->size; off+=lpbk->block_size)
  {
    err = lseek(lpbk->bfile_fd, off, SEEK_SET) < 0;
    if((err = FLA_ERR_ERRNO(err, "lseek()")))
    {
      goto free_buf;
    }

    memset(buf, '0', lpbk->block_size);
    *buf = off;

    err = write(lpbk->bfile_fd, buf, lpbk->block_size) < 0;
    if((err = FLA_ERR_ERRNO(err, "write()")))
    {
      goto free_buf;
    }
  }

free_buf:
  free(buf);

exit:
  return err;
}

int
fla_ut_assert_equal_within_file_char(const int file_fd, const int file_offset,
                                     const char * expected_str)
{
  int err = 0;
  char * read_bytes = NULL;
  size_t str_size = strlen(expected_str);

  read_bytes = malloc(str_size);
  if(FLA_ERR(!read_bytes, "malloc()"))
  {
    err = -ENOMEM;
    goto exit;
  }

  err = lseek(file_fd, file_offset, SEEK_SET) < 0;
  if((err = FLA_ERR_ERRNO(err, "lseek()")))
  {
    goto free_read_bytes;
  }

  err = read(file_fd, read_bytes, str_size) < 0;
  if((err = FLA_ERR_ERRNO(err, "read()")))
  {
    goto free_read_bytes;
  }

  char * read_str = (char*)read_bytes;
  err = strncmp(expected_str, read_str, str_size);
  if (err)
  {
    fprintf(stderr, "Test Error: Strings are not the same. (%s) (%s)\n",
            expected_str, read_str);
    goto free_read_bytes;
  }

free_read_bytes:
  free(read_bytes);

exit:
  return err;
}

int
fla_ut_count_char_in_buf(const char c, const char * buf, const int size)
{
  int count = 0;
  for(int i = 0; i < size; ++i)
  {
    if(*(buf + i) == c)
    {
      count++;
    }
  }
  return count;
}

int
fla_ut_count_char_in_file(const char c, const int file_fd, const size_t file_offset,
                          const size_t size, int * count)
{
  int err = 0, buf_size = 512, read_bytes, read_bytes_t = 0;
  char * buf = NULL;

  buf = malloc(buf_size);
  if(FLA_ERR(!buf, "malloc()"))
  {
    err = -ENOMEM;
    goto exit;
  }

  err = lseek(file_fd, file_offset, SEEK_SET) < 0;
  if((err = FLA_ERR_ERRNO(err, "lseek()")))
  {
    goto free_buf;
  }

  (*count) = 0;
  do
  {
    read_bytes = read(file_fd, buf, buf_size);
    if((err = FLA_ERR_ERRNO(read_bytes == -1, "read()")))
    {
      goto free_buf;
    }

    (*count) += fla_ut_count_char_in_buf(c, buf, read_bytes);
  }
  while(read_bytes_t <= size && read_bytes != 0);


free_buf:
  free(buf);

exit:
  return err;
}

#define fla_ut_assert_equal_within_file_numeric_T(T) \
  int fla_ut_assert_equal_within_file_##T(const int file_fd, const int file_offset, const T expected_val) \
{ \
  int err = 0; \
  char * read_bytes = NULL; \
  size_t val_size = sizeof(T); \
  \
  read_bytes = malloc(val_size); \
  if(FLA_ERR(!read_bytes, "malloc()")){ \
    err = -ENOMEM; \
    goto exit; \
  } \
  \
  err = lseek(file_fd, file_offset, SEEK_SET) < 0; \
  if((err = FLA_ERR_ERRNO(err, "lseek()"))){goto free_read_bytes;} \
  \
  err = read(file_fd, read_bytes, val_size) < 0; \
  if((err = FLA_ERR_ERRNO(err, "read()"))){goto free_read_bytes;} \
  \
  T read_val = *(T*)read_bytes; \
  err = expected_val - read_val; \
  if(err) {\
    const char * msg = "Test Error: %s's types are not equal. (%ld) (%ld)\n"; \
    fprintf(stderr, msg, #T, (long int)expected_val, (long int)read_val); \
    goto free_read_bytes; \
  } \
  \
  free_read_bytes: \
  free(read_bytes); \
  exit: \
  return err; \
}

/*define fla_ut_assert_equal_int32_t*/
fla_ut_assert_equal_within_file_numeric_T(int32_t);
/*define fla_ut_assert_equal_int64_t*/
fla_ut_assert_equal_within_file_numeric_T(int64_t);

int
fla_expr_assert(char *expr_s, int expr_result, char *err_msg, const char *func,
                const int line, ...)
{
  va_list args;
  va_start(args, line);
  if (!expr_result)
  {
    fprintf(stderr, "FAIL(%s: %d): %s, ", func, line, expr_s);
    vfprintf(stderr, err_msg, args);
    fprintf(stderr, "\n");
    fflush(stderr);
  }
  va_end(args);
  return !expr_result;
}

int
fla_ut_lpbk_fs_create(uint64_t lb_nbytes, uint64_t nblocks, uint32_t slab_nlb,
                      uint32_t npools,
                      struct fla_ut_lpbk **lpbk, struct flexalloc **fs)
{
  struct fla_mkfs_p mkfs_params = {0};
  struct fla_open_opts open_opts = {0};
  int err, ret;

  err = fla_ut_lpbk_dev_alloc(lb_nbytes, nblocks, lpbk);
  if(FLA_ERR(err, "fla_ut_lpbk_dev_alloc()"))
  {
    goto exit;
  }

  mkfs_params.dev_uri = (*lpbk)->dev_name;
  mkfs_params.slab_nlb = slab_nlb;
  mkfs_params.npools = npools;
  err = fla_mkfs(&mkfs_params);
  if(FLA_ERR(err, "fla_mkfs()"))
  {
    goto teardown_lpbk;
  }

  open_opts.dev_uri = (*lpbk)->dev_name;
  err = fla_open(&open_opts, fs);
  if(FLA_ERR(err, "fla_open()"))
  {
    goto teardown_lpbk;
  }

  return 0; // success

teardown_lpbk:
  ret = fla_ut_lpbk_dev_free(*lpbk);
  err |= FLA_ERR(ret, "fla_ut_lpbk_dev_free()");
exit:
  return err;
}

int
fla_ut_lpbk_fs_destroy(struct fla_ut_lpbk *lpbk, struct flexalloc *fs)
{
  int err = 0, ret;
  ret = fla_close(fs);
  err |= FLA_ERR(ret, "fla_close()");

  ret = fla_ut_lpbk_dev_free(lpbk);
  err |= FLA_ERR(ret, "fla_ut_lpbk_dev_free()");
  return err;
}

bool
is_globalenv_set(char const * glb)
{
  return getenv(glb) != NULL;
}

int
fla_ut_dev_init(uint64_t disk_min_blocks, struct fla_ut_dev *dev)
{
  struct xnvme_dev *xdev;
  int err = 0;
  dev->_is_zns = 0;
  dev->_md_dev_uri = NULL;

  if(is_globalenv_set("FLA_TEST_DEV"))
  {
    dev->_dev_uri = getenv("FLA_TEST_DEV");
    dev->_md_dev_uri = getenv("FLA_TEST_MD_DEV");
    dev->_is_loop = 0;
    err = fla_xne_dev_open(dev->_dev_uri, NULL, &xdev);
    if (FLA_ERR(err, "fla_xne_dev_open()"))
    {
      return FLA_ERR_ERROR;
    }
    dev->lb_nbytes = fla_xne_dev_lba_nbytes(xdev);
    dev->nblocks = FLA_CEIL_DIV(fla_xne_dev_tbytes(xdev), dev->lb_nbytes);
    if (dev->_md_dev_uri)
    {
      if (fla_xne_dev_type(xdev) == XNVME_GEO_ZONED)
      {
        dev->nzones = fla_xne_dev_znd_zones(xdev);
        dev->nsect_zn = fla_xne_dev_znd_sect(xdev);
        dev->_is_zns = 1;
      }
    }

    xnvme_dev_close(xdev);
    if (FLA_ERR(dev->nblocks < disk_min_blocks,
                "Backing device disk too small for test requirements"))
    {
      return FLA_ERR_ERROR;
    }
  }
  else
  {
    dev->_is_loop = 1;
    dev->lb_nbytes = 512;
    if(getenv("FLA_LOOP_LBS") != NULL)
    {
      errno = 0;
      dev->lb_nbytes = strtol(getenv("FLA_LOOP_LBS"), NULL, 10);
      if ((err = FLA_ERR(errno != 0,"Could not properly convert FLA_LOOP_LBS")))
        return err;
    }
    dev->nblocks = disk_min_blocks;
    err = fla_ut_lpbk_dev_alloc(dev->lb_nbytes, disk_min_blocks, &dev->_loop);
    if(FLA_ERR(err, "fla_ut_lpbk_dev_alloc()"))
    {
      return err;
    }
    dev->_dev_uri = dev->_loop->dev_name;
  }
  return 0;
}

int
fla_ut_dev_teardown(struct fla_ut_dev *dev)
{
  int err = 0;
  if (dev->_is_loop)
  {
    err |= fla_ut_lpbk_dev_free(dev->_loop);
    if(FLA_ERR(err, "fla_ut_lpbk_dev_free()"))
    {
      return err;
    }
  }
  return 0;
}

int
fla_ut_dev_use_device(struct fla_ut_dev *dev)
{
  return dev->_is_loop;
}

int
fla_t_round_slab_size(struct fla_ut_dev * test_dev, uint32_t * slab_min_blocks)
{
  int err = 0;
  struct xnvme_dev * dev;
  err = fla_xne_dev_open(test_dev->_dev_uri, NULL, &dev);

  if (fla_xne_dev_type(dev) == XNVME_GEO_ZONED)
  {
    uint64_t zsz = fla_xne_dev_znd_sect(dev);
    uint64_t div_res = *slab_min_blocks % zsz;
    if (div_res > 0 )
      *slab_min_blocks += (zsz - div_res);
  }

  fla_xne_dev_close(dev);
  return err;
}

int
fla_ut_fs_create(uint32_t slab_min_blocks, uint32_t npools,
                 struct fla_ut_dev *dev, struct flexalloc **fs)
{
  struct fla_mkfs_p mkfs_params = {0};
  struct fla_open_opts open_opts = {0};
  int err = 0, ret;

  if (dev->_is_loop)
  {
    FLA_VBS_PRINTF("fla_ut_fs_create: use loopback device '%s'\n", dev->_dev_uri);
  }
  else
  {
    FLA_VBS_PRINTF("fla_ut_fs_create: use device '%s'\n", dev->_dev_uri);
    if (dev->_md_dev_uri)
    {
      FLA_VBS_PRINTF("fla_ut_fs_create: zns md device '%s'\n", dev->_md_dev_uri);
    }
  }

  fla_t_round_slab_size(dev, &slab_min_blocks);
  mkfs_params.dev_uri = (char *)dev->_dev_uri;
  mkfs_params.md_dev_uri = (char *)dev->_md_dev_uri;
  mkfs_params.slab_nlb = slab_min_blocks;
  mkfs_params.npools = npools;
  err = fla_mkfs(&mkfs_params);
  if(FLA_ERR(err, "fla_mkfs()"))
  {
    goto teardown;
  }

  open_opts.dev_uri = (char *)dev->_dev_uri;
  open_opts.md_dev_uri = (char *)dev->_md_dev_uri;
  err = fla_open(&open_opts, fs);
  if(FLA_ERR(err, "fla_open()"))
  {
    goto teardown;
  }

  return 0; // success

teardown:
  if (dev->_is_loop)
  {
    ret = fla_ut_lpbk_dev_free(dev->_loop);
    err |= FLA_ERR(ret, "fla_ut_lpbk_dev_free()");
  }

  return err;
}

int
fla_ut_fs_teardown(struct flexalloc *fs)
{
  int err = 0;
  err = fla_close(fs);
  // only report the error (if it happens), we want to continue cleanup as best we can
  FLA_ERR(err, "fla_close() - failed to properly close device");

  return err;
}

void
fla_t_fill_buf_random(char * buf, const size_t size)
{
  static char * alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
  int rnd_num;

  for(int rnd_idx = 0; rnd_idx < size ; ++rnd_idx)
  {
    rnd_num = rand() % strlen(alphabet);
    *(buf + rnd_idx) = *(alphabet + rnd_num);
  }
  *(buf + size) = '\0';
}

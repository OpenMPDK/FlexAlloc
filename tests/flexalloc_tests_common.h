// Copyright (C) 2021 Joel Granados <j.granados@samsung.com>
// Copyright (C) 2021 Jesper Devantier <j.devantier@samsung.com>
// Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>

#ifndef __FLEXALLOC_TESTS_COMMON_H
#define __FLEXALLOC_TESTS_COMMON_H

#include <stdint.h>
#include "flexalloc_xnvme_env.h"
#include "flexalloc_mm.h"

/**
 * Loopback elements
 */
#define FLA_UT_DEV_NAME_SIZE 1024
#define FLA_UT_BACKING_FILE_NAME_SIZE 255

#define FLA_TEST_LPBK_REMOVE_BFILE "FLA_TEST_LPBK_REMOVE_BFILE"
/**
 * Loop back devices
 */
struct fla_ut_lpbk
{
  uint64_t block_size;  // block size, in bytes
  uint64_t size;        // total size, in bytes
  int bfile_fd;         // file descriptor of backing file
  char * bfile_name;    // name of backing file
  int dev_fd;           // file descriptor of device file
  char * dev_name;      // name of device
};

struct fla_ut_dev
{
  /// block size, in bytes (can be read after initialization)
  uint64_t lb_nbytes;
  /// number of logical blocks on device (can be read after initialization)
  uint64_t nblocks;
  // ZNS
  /// Number of zones on zns device
  uint32_t nzones;
  /// Number of sectors per zone
  uint64_t nsect_zn;

  // internal
  /// device node path
  const char *_dev_uri;
  /// reference to loopback structure (if loopback device)
  struct fla_ut_lpbk *_loop;
  /// true iff. selected device is a loopback device
  uint8_t _is_loop;
  // ZNS
  /// ZNS require a separate MD device
  const char *_md_dev_uri;
  uint8_t _is_zns;
};

#define FLA_TEST_SKIP_RETCODE 77

bool
is_globalenv_set(char const * glb);

int
fla_ut_lpbk_dev_alloc(uint64_t block_size, uint64_t nblocks, struct fla_ut_lpbk **loop);

int
fla_ut_lpbk_dev_free(struct fla_ut_lpbk *loop);

int
fla_ut_lpbk_fs_create(uint64_t lb_nbytes, uint64_t nblocks, uint32_t slab_nlb,
                      uint32_t npools,
                      struct fla_ut_lpbk **lpbk, struct flexalloc **fs);

int
fla_ut_lpbk_fs_destroy(struct fla_ut_lpbk *lpbk, struct flexalloc *fs);

/**
 * True iff fla_ut_fs_create would use a device.
 *
 * Use this to determine if fla_ut_fs_create() would use a backing device.
 * Tests requiring a certain type of device can use this to check and return
 * FLA_TEST_SKIP_RECODE to skip execution if needed.
 */
int
fla_ut_dev_use_device(struct fla_ut_dev *dev);

/**
 * Get a wrapped device instance for testing.
 *
 * @param disk_min_512byte_blocks minimum number of 512B blocks required for test
 *        (if using a loopback device, this becomes the disk size)
 * @param dev wrapped device
 */
int
fla_ut_dev_init(uint64_t disk_min_512byte_blocks, struct fla_ut_dev *dev);

/**
 * Release use of test device
 *
 * @param dev test device
 */
int
fla_ut_dev_teardown(struct fla_ut_dev *dev);

/**
 * Create a flexalloc instance using device if FLA_TEST_DEV is set, otherwise using loopback.
 *
 * @param slab_min_blocks minimum number of blocks
 * @param npools number of pools to allocate space for in flexalloc
 * @param dev contains information related to which device type was selected,
 *            fields not prepended by underscore may be read.
 * @param fs flexalloc handle
 * @return 0 on success, error otherwise.
 */
int
fla_ut_fs_create(uint32_t slab_min_blocks, uint32_t npools,
                 struct fla_ut_dev *dev, struct flexalloc **fs);

/**
 * Release flexalloc instance and backing (loopback?) device.
 *
 * @param dev unit test device wrapper
 * @param fs flexalloc handle
 *
 * @return 0 on success, non-zero on error, cleanup still proceeds
 */
int
fla_ut_fs_teardown(struct flexalloc *fs);

int
fla_ut_temp_file_create(const int size, char * created_name);

int
fla_ut_lpbk_overwrite(const char c, struct fla_ut_lpbk * lpbk);


int
fla_ut_lpbk_offs_blk_fill(struct fla_ut_lpbk * lpbk);

void
fla_t_fill_buf_random(char * buf, const size_t size);

/**
 * Assert functions for the testing frame work
 */

/**
 * Compares file offset with char. Uses strlen.
 *
 * @param file_fd file descriptor (file is already open)
 * @param file_offset lseek to this offset
 * @param expected_str strcmp with this string. Use the size of string to compare
 *
 * @return 0 on success, !=0 otherwise.
 */
int fla_ut_assert_equal_within_file_char(const int file_fd, const int file_offset,
    const char * expected_str);

int fla_ut_count_char_in_buf(const char c, const char * buf, const int size);
int fla_ut_count_char_in_file(const char c, const int file_fd, const size_t file_offset,
                              const size_t size, int * i);

/**
 * Numerical comparison. It is implemented as a macro
 *
 * @param file_fd file descriptor (file is already open)
 * @param file_offset lseek to this offset
 * @param expected_val expected numerical value
 *
 * @return 0 on success, !=0 otherwise.
 */
int fla_ut_assert_equal_within_file_int32_t(const int file_fd, const int file_offset,
    const int32_t expected_val);
int fla_ut_assert_equal_within_file_int64_t(const int file_fd, const int file_offset,
    const int64_t expected_val);

int
fla_expr_assert(char *expr_s, int expr_result, char *err_msg, const char *func,
                const int line, ...);
/**
 * Assert truthfulness of expression.
 *
 * @param expr any C expression which evaluates to a boolean value.
 * @param err_msg error message to display in case the assertion fails
 * @return 0 on success, != 0 otherwise.
 */
#define FLA_ASSERT(expr, err_msg)                   \
  fla_expr_assert(#expr, expr, err_msg, __func__, __LINE__)

/**
 * Assert truthfulness of expression.
 *
 * @param expr any C expression - ordinary truthyness rules apply
 * @param err_msg error format string
 * @param ... arguments to formatted string
 * @return 0 on success, != 0 otherwise.
 */
#define FLA_ASSERTF(expr, err_msg, ...)       \
  fla_expr_assert(#expr, expr, err_msg, __func__, __LINE__,  __VA_ARGS__)

int
fla_t_round_slab_size(struct fla_ut_dev * test_dev, uint32_t * slab_min_blocks);

#endif /* __FLEXALLOC_TESTS_COMMON_H */

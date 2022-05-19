// Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>

#ifndef __LIBFLAN_H_
#define __LIBFLAN_H_
#include <limits.h>
#include <libflexalloc.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLAN_OBJ_NAME_LEN_MAX 112
#define FLAN_DHANDLE_NONE UINT_MAX
#define FLAN_OPEN_FLAG_CREATE 0x1
#define FLAN_OPEN_FLAG_READ 0x2
#define FLAN_OPEN_FLAG_WRITE 0x4
#define FLAN_MAX_OPEN_OBJECTS 16384
#define FLAN_APPEND_SIZE 2097152

struct flan_oinfo
{
  uint64_t size;
  struct fla_object fla_oh;
  char name[FLAN_OBJ_NAME_LEN_MAX];
};

struct flan_dirhandle
{
  char *buf;
  unsigned int cur;
  struct fla_object fla_oh;
  unsigned int dev_last;
};

struct flan_handle
{
  struct flexalloc *fs;
  struct fla_pool *ph;
  uint32_t append_sz;
  bool is_zns;
  bool is_dirty;
};

struct flan_ohandle
{
	struct flan_oinfo oinfo;
	struct fla_object flan_oh;
	uint32_t oinfo_off;
	char *append_buf;
	char *read_buf;
	uint64_t append_off;
	uint64_t read_buf_off;
	uint32_t use_count;
	uint32_t o_flags;
	bool frozen;
};


int flan_init(const char *dev_uri, const char *mddev_uri, const char *pool_name, uint64_t obj_sz,
                struct flan_handle **flanh);
void flan_reset_pool_dir();
struct flan_oinfo* flan_get_oinfo(struct flan_handle *flanh, bool create);
int flan_object_open(const char *filename, struct flan_handle *flanh, uint64_t *oh, int flags);
int flan_object_delete(const char *filename, struct flan_handle *flanh);
ssize_t flan_object_read(uint64_t oh, void *buf, size_t offset, size_t len, struct flan_handle *flanh);
int flan_object_write(uint64_t oh, void *buf, size_t offset, size_t len, struct flan_handle *flanh);
int flan_object_close(uint64_t oh, struct flan_handle *flanh);
void flan_close(struct flan_handle *flanh);
void *flan_buf_alloc(size_t nbytes, struct flan_handle *flanh);
uint32_t flan_dev_bs(struct flan_handle *flanh);
int flan_object_rename(const char *oldname, const char *newname, struct flan_handle *flanh);
int flan_sync(struct flan_handle *flanh);
bool flan_is_zns(struct flan_handle *flanh);
struct flan_oinfo *flan_find_oinfo(struct flan_handle *flanh, const char *name, uint32_t *cur);
int flan_pool_set_strp(struct flan_handle *flanh, uint32_t num_strp, uint32_t strp_sz);
#ifdef __cplusplus
}
#endif

#endif // __LIBFLAN_H_


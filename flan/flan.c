// Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <libgen.h>

#include "flan.h"

struct flan_dirhandle flan_dir;
unsigned int num_objects;
unsigned int flan_obj_sz;
unsigned int flan_oi_ob;
struct flan_handle *root;

struct flan_ohandle flan_otable[FLAN_MAX_OPEN_OBJECTS];

void flan_cleanup(void)
{
  flan_close(root);
}

int flan_pool_set_strp(struct flan_handle *flanh, uint32_t num_strp, uint32_t strp_sz)
{
  return fla_pool_set_strp(flanh->fs, flanh->ph, num_strp, strp_sz);
}

int flan_init(const char *dev_uri, const char *mddev_uri, const char *pool_name, uint64_t objsz,
                struct flan_handle **flanh)
{
  struct fla_object robj;
  int ret = 0;
  uint32_t obj_nlb;
  struct fla_open_opts open_opts = {0};
  struct xnvme_opts x_opts = xnvme_opts_default();

  *flanh = malloc(sizeof(struct flan_handle));
  memset(*flanh, 0, sizeof(struct flan_handle));
  memset(flan_otable, 0, sizeof(struct flan_ohandle) * FLAN_MAX_OPEN_OBJECTS);

  open_opts.dev_uri = dev_uri;
  open_opts.opts = &x_opts;
  open_opts.opts->async = "io_uring_cmd";
  if (mddev_uri)
	  open_opts.md_dev_uri = mddev_uri;

  ret = fla_open(&open_opts, &((*flanh)->fs));
  if (ret)
  {
    printf("Error fla opening dev:%s,md_dev:%s\n", dev_uri, mddev_uri);
    goto out_free;
  }

  (*flanh)->is_zns = fla_fs_zns((*flanh)->fs);
  if (objsz > FLAN_APPEND_SIZE)
    (*flanh)->append_sz = FLAN_APPEND_SIZE;
  else
    (*flanh)->append_sz = objsz;

  obj_nlb = objsz / fla_fs_lb_nbytes((*flanh)->fs);
  ret = fla_pool_open((*flanh)->fs, pool_name, &((*flanh)->ph));
  if (ret == -1)
	  ret = fla_pool_create((*flanh)->fs, pool_name, strlen(pool_name), obj_nlb, &((*flanh)->ph));

  if (ret)
  {
    printf("Error opening fla pool:%s\n", pool_name);
    goto out_free;
  }

  flan_obj_sz = obj_nlb * fla_fs_lb_nbytes((*flanh)->fs);
  flan_oi_ob = flan_obj_sz / sizeof(struct flan_oinfo);
  flan_dir.buf = fla_buf_alloc((*flanh)->fs, flan_obj_sz);
  if (!flan_dir.buf)
  {
    printf("fla_buf_alloc\n");
    goto out_free;
  }

  memset((void *)flan_dir.buf, 0, flan_obj_sz);
  flan_dir.cur = FLAN_DHANDLE_NONE;
  num_objects = 0;

  ret = fla_pool_get_root_object((*flanh)->fs, (*flanh)->ph, &robj);
  flan_dir.fla_oh = robj;
  if (ret) // Root object not set
  {
    // Grab a object from the pool, zero it out, and set it to root
    ret = fla_object_create((*flanh)->fs, (*flanh)->ph, &robj);
    if (ret)
    {
      printf("Error allocating root object\n");
      goto out_free_buf;
    }

    if (!(*flanh)->is_zns)
    {
      ret = fla_object_write((*flanh)->fs, (*flanh)->ph, &robj, flan_dir.buf, 0, flan_obj_sz);
      if (ret)
      {
        printf("Error initial write md object \n");
        goto out_free_buf;
      }
    }

    // Set the acquired object to the root object
    ret =  fla_pool_set_root_object((*flanh)->fs, (*flanh)->ph, &robj, false);
    flan_dir.fla_oh = robj;
    (*flanh)->is_dirty = false; // Metadata is currently clean
  }
  else
  {
    ret = fla_object_read((*flanh)->fs, (*flanh)->ph, &robj, flan_dir.buf, 0, flan_obj_sz);
  }

  flan_dir.cur = 0;
  root = *flanh;
  atexit(flan_cleanup);
  return ret;

out_free_buf:
  free(flan_dir.buf);
out_free:
  free(*flanh);

  return ret;
}

void flan_reset_pool_dir()
{
  flan_dir.cur = FLAN_DHANDLE_NONE;
}

int flan_init_dirhandle(struct flan_handle *flanh)
{
  struct fla_object robj;
  int ret = 0;

  if (flan_dir.cur == FLAN_DHANDLE_NONE)
  {
    flan_dir.cur = 0;
    if (!flanh->is_zns)
    {
      ret = fla_pool_get_root_object(flanh->fs, flanh->ph, &robj);
      if (ret)
      {
        printf("Error getting root object\n");
        goto out;
      }

      ret = fla_object_read(flanh->fs, flanh->ph, &robj, flan_dir.buf, 0, flan_obj_sz);
      if (ret)
      {
        printf("Error reading root object\n");
        goto out;
      }

      flan_dir.fla_oh = robj;
    }
  }

out:
  return ret;

}

struct flan_oinfo* flan_get_oinfo(struct flan_handle *flanh, bool create)
{
  int ret = 0;
  struct flan_oinfo *oinfo;

  ret = flan_init_dirhandle(flanh);
  if (ret)
  {
    printf("Error initializing dirhandle\n");
    return NULL;
  }

  // We ran out of md objects
  if(flan_dir.cur && !(flan_dir.cur % flan_oi_ob))
    return NULL;

  if (!((flan_dir.cur + 1) % flan_oi_ob))
  {
    oinfo = ((struct flan_oinfo *)flan_dir.buf + (flan_oi_ob - 1));
    // Check if next entry is a pointer to another object
    if (oinfo->size == UINT64_MAX)
    {
      flan_dir.fla_oh = oinfo->fla_oh;
      fla_object_read(flanh->fs, flanh->ph, &oinfo->fla_oh, flan_dir.buf, 0, flan_obj_sz);
      // Skip over the oinfo object which points to the next object used for MD
      flan_dir.cur++;
    }
    else if(create)// Lets try to allocate a new object
    {
      struct fla_object new_md_obj;
      // Grab a object from the pool, zero it out, and set it to root
      ret = fla_object_create(flanh->fs, flanh->ph, &new_md_obj);
      if (!ret)
      {
        oinfo->size = UINT64_MAX;
        oinfo->fla_oh = new_md_obj;
        fla_object_write(flanh->fs, flanh->ph, &flan_dir.fla_oh, flan_dir.buf, 0, flan_obj_sz);
        memset((void *)flan_dir.buf, 0, flan_obj_sz);
        flan_dir.fla_oh = new_md_obj;
        // Skip over the oinfo object which points to the next MD object
        flan_dir.cur++;
      }
      else
      {
        printf("Error allocating MD object bailing\n");
        exit(0);
      }
    }
  }

  oinfo = ((struct flan_oinfo *)flan_dir.buf + (flan_dir.cur % flan_oi_ob));
  flan_dir.cur++;
  return oinfo;
}

int flan_object_create(const char *name, struct flan_handle *flanh, struct flan_oinfo *oinfo)
{
  int ret;
  unsigned int namelen = strlen(name);
  struct fla_object fla_obj;

  if (namelen > FLAN_OBJ_NAME_LEN_MAX)
  {
    printf("flan obj create Name:%s longer than max len of:%d\n", name,  FLAN_OBJ_NAME_LEN_MAX);
    return -EINVAL;
  }

  ret = fla_object_create(flanh->fs, flanh->ph, &fla_obj);
  if (ret)
  {
    printf("Error allocating fla object\n");
    return ret;
  }

  memcpy(oinfo->name, name, namelen + 1);
  oinfo->size = 0;
  oinfo->fla_oh = fla_obj;

  if (!flanh->is_zns)
  {
    // TOOD make sure this is safe with object handle table now having private copies
    ret = fla_object_write(flanh->fs, flanh->ph, &flan_dir.fla_oh, flan_dir.buf, 0, flan_obj_sz);
    if (ret)
    {
      printf("Error writing md for object :%d\n", num_objects);
      exit(0);
    }
  }
  else
    flanh->is_dirty = true;

  num_objects++;

  return ret;

}

struct flan_oinfo *flan_find_oinfo(struct flan_handle *flanh, const char *name)
{
  struct flan_oinfo *oinfo;

  flan_reset_pool_dir();
  while (((oinfo = flan_get_oinfo(flanh, false)) != NULL) &&
         strncmp(oinfo->name, name, FLAN_OBJ_NAME_LEN_MAX));

  return oinfo;
}

struct flan_oinfo *flan_find_first_free_oinfo(struct flan_handle *flanh)
{
  struct flan_oinfo *oinfo;

  flan_reset_pool_dir();
  while (((oinfo = flan_get_oinfo(flanh, true)) != NULL) &&
         strlen(oinfo->name) > 0);

  return oinfo;
}

uint64_t flan_otable_search(const char *name, uint64_t *ff)
{
  bool oh_set = false;
  uint64_t oh_num = FLAN_MAX_OPEN_OBJECTS;

  // Seach through the open objects table
  for (oh_num = 0; oh_num < FLAN_MAX_OPEN_OBJECTS; oh_num++)
  {
    // Blank entry
    if (!strlen(flan_otable[oh_num].oinfo.name))
    {
      if (!oh_set)
      {
        if (ff)
          *ff = oh_num;
        
        oh_set = true;
      }

      continue;

    }

    // Entry with a matching name return the entry
    if (!strncmp(name, flan_otable[oh_num].oinfo.name, FLAN_OBJ_NAME_LEN_MAX))
    {
      return oh_num;
    }
  }

  if (!oh_set)
    *ff = FLAN_MAX_OPEN_OBJECTS;

  return oh_num;
}

int flan_object_open(const char *name, struct flan_handle *flanh, uint64_t *oh, int flags)
{
  struct flan_oinfo *oinfo;
  int ret = 0;
  uint64_t oh_num;
  uint64_t ff_oh;
  struct fla_object *noh;
  uint32_t bs = flanh->append_sz;

  oh_num = flan_otable_search(basename((char *)name), &ff_oh);
 
  // The object is already open increase ref count and return object handle
  if (oh_num < FLAN_MAX_OPEN_OBJECTS)
  {
    *oh = oh_num;
    flan_otable[oh_num].use_count++;
    return 0;
  }

  if (ff_oh == FLAN_MAX_OPEN_OBJECTS)
  {
    printf("Open objects hit limit of %d\n", FLAN_MAX_OPEN_OBJECTS);
    return -EINVAL;
  }

  // Search through all the on disk MD
  oinfo = flan_find_oinfo(flanh, basename((char *)name));
  if (!oinfo && (flags & FLAN_OPEN_FLAG_CREATE))
    oinfo = flan_find_first_free_oinfo(flanh);

  if (!oinfo)
    return -EINVAL;

  if ((strlen(oinfo->name) == 0) && (flags & FLAN_OPEN_FLAG_CREATE))
  {
    ret = flan_object_create(basename((char *)name), flanh, oinfo);
    if (ret)
    {
      printf("flan object open: object create error\n");
      return ret;
    }
  }

  memcpy(&flan_otable[ff_oh].oinfo, oinfo, sizeof(struct flan_oinfo));
  flan_otable[ff_oh].flan_oh = flan_dir.fla_oh;
  flan_otable[ff_oh].oinfo_off = (flan_dir.cur - 1) % flan_oi_ob; // Convert to byte offset for update
  if (flags & FLAN_OPEN_FLAG_WRITE)
    flan_otable[ff_oh].append_off = flan_otable[ff_oh].oinfo.size; // Verify zero on new entry
  else if (flags & FLAN_OPEN_FLAG_READ)
    flan_otable[ff_oh].read_buf_off = UINT64_MAX;

  flan_otable[ff_oh].use_count++;
  flan_otable[ff_oh].append_buf = fla_buf_alloc(flanh->fs, flanh->append_sz);

  if (flags & FLAN_OPEN_FLAG_READ)
    flan_otable[ff_oh].read_buf = flan_otable[ff_oh].append_buf;
  
  flan_otable[ff_oh].o_flags = flags;

  memset(flan_otable[ff_oh].append_buf, 0, bs);

  if (!flan_otable[ff_oh].append_buf)
  {
    printf("Object open unable to allocate append buf\n");
    return -EINVAL;
  }

  // Read the data into the append buffer
  if (flan_otable[ff_oh].append_off < flan_obj_sz * 64 && flags && FLAN_OPEN_FLAG_WRITE)
  {
    noh = &oinfo->fla_oh;
    fla_object_read(flanh->fs, flanh->ph, noh, flan_otable[ff_oh].append_buf,
                    (flan_otable[ff_oh].append_off / bs) * bs, bs);
  }

  // Freeze a zns file that has been previously appended
  if (flan_otable[ff_oh].append_off % flanh->append_sz && flanh->is_zns)
    flan_otable[ff_oh].frozen = true;

  *oh = ff_oh;
  return ret;
}

int flan_object_delete(const char *name, struct flan_handle *flanh)
{
  uint64_t oh = flan_otable_search(basename((char *)name), NULL);
  struct flan_oinfo *oinfo = flan_find_oinfo(flanh, basename((char *)name));
  
  // Invalidate any open handles, but keep use count to free ohandle when all openers have closed
  if (oh != FLAN_MAX_OPEN_OBJECTS)
  {
    free(flan_otable[oh].append_buf);
    memset(&flan_otable[oh], 0, sizeof(struct flan_ohandle) - sizeof(uint32_t));
  }

  if (!oinfo)
  {
    return -EINVAL;
  }

  memset(oinfo->name, 0, FLAN_OBJ_NAME_LEN_MAX);
  oinfo->size = 0;
  oinfo->fla_oh.slab_id = UINT32_MAX;
  oinfo->fla_oh.entry_ndx = UINT32_MAX;
  num_objects--;

  if (!flanh->is_zns)
  {
    if (fla_object_write(flanh->fs, flanh->ph, &flan_dir.fla_oh, flan_dir.buf, 0, flan_obj_sz))
    {
      printf("Error writing md during delete for object:%s\n", name);
      exit(0);
    }
  }
  else
    flanh->is_dirty = true;

  return 0;
}

void flan_otable_close(struct flan_handle *flanh)
{
  uint64_t oh_num;

  for (oh_num = 0; oh_num < FLAN_MAX_OPEN_OBJECTS; oh_num++)
  {
    if (strlen(flan_otable[oh_num].oinfo.name))
      flan_object_close(oh_num, flanh);
  }
}

void flan_close(struct flan_handle *flanh)
{
  if (!flanh)
    return;

  flan_otable_close(flanh);

  if (flanh->is_zns && !flanh->is_dirty) // Also clean case, TODO make this independent of ZNS
    goto free;

  if (fla_object_write(flanh->fs, flanh->ph, &flan_dir.fla_oh, flan_dir.buf, 0, flan_obj_sz))
  {
    printf("Error writing md during close\n");
    exit(0);
  }

free:
  if (flan_dir.buf)
    fla_buf_free(flanh->fs, flan_dir.buf);
  
  fla_pool_close(flanh->fs, flanh->ph);
  fla_close(flanh->fs);
  free(flanh);
  root = NULL;
}

int flan_object_unaligned_read(uint64_t oh, struct fla_object *noh, void *buf,
                                 size_t offset, size_t len, struct flan_handle *flanh)
{
  void *al_buf;
  size_t al_offset, al_len, tail_len = 0, tail_start = 0;
  int ret = 0;
  uint32_t bs = flanh->append_sz;

  al_offset = offset - (offset % bs);
  al_len = len + (bs - (len % bs));
  if (al_offset + al_len < offset + len)
	  al_len += bs;

  al_buf = flan_buf_alloc(al_len, flanh);
  if (!al_buf)
    return -ENOMEM;

  if (al_offset + al_len > flan_otable[oh].append_off && flanh->is_zns)
  {
    al_len -= bs;
    if (al_len)
      tail_len = (offset + len) - (al_offset + al_len);
    else
      tail_len = len;
  }

  if (al_len)
    ret = fla_object_read(flanh->fs, flanh->ph, noh, al_buf, al_offset, al_len);

  if (ret)
  {
    printf("Whoops unaligned object read went wrong\n");
    goto out_free;
  }

  if (al_offset / bs == flan_otable[oh].append_off / bs)
    tail_start = offset % bs;

  if (tail_len)
  {
    if (al_len)
      memcpy(buf, al_buf + (offset % bs), al_len - (offset % bs));

    if (al_len)
      memcpy(buf + al_len - (offset % bs), flan_otable[oh].append_buf + tail_start, tail_len);
    else
      memcpy(buf, flan_otable[oh].append_buf + tail_start, tail_len);
  }
  else
  {
    memcpy(buf, al_buf + (offset % bs), len);
  }

out_free:
  free(al_buf);
  return ret;
}

ssize_t flan_object_read_r(uint64_t oh, void *buf, size_t offset, size_t len,
                             struct flan_handle *flanh, struct flan_oinfo *oinfo)
{
  uint64_t *rb_off = &flan_otable[oh].read_buf_off;
  char *rb = flan_otable[oh].read_buf;
  char *bufpos = buf;
  struct fla_object *flaobj = &oinfo->fla_oh;
  size_t from_buffer = 0, toRead = len, append_sz = FLAN_APPEND_SIZE;

  // Read in the correct block if the starting address does not fall within the buffer
  if (offset < *rb_off || offset >= (*rb_off + FLAN_APPEND_SIZE))
  {
    *rb_off = (offset / FLAN_APPEND_SIZE) * FLAN_APPEND_SIZE;
    fla_object_read(flanh->fs, flanh->ph, flaobj, rb, *rb_off, FLAN_APPEND_SIZE);
  }

  // Read completely contained in buffer
  if (offset >= *rb_off && offset + len <= *rb_off + FLAN_APPEND_SIZE)
  {
    memcpy(bufpos, rb + offset % FLAN_APPEND_SIZE, len);
    return len;
  }

  from_buffer = FLAN_APPEND_SIZE - (offset % FLAN_APPEND_SIZE);
  memcpy(bufpos, rb + offset % FLAN_APPEND_SIZE, from_buffer);

  toRead -= from_buffer;
  bufpos += from_buffer;
  offset += from_buffer;

  // TODO set the rb_off if it hasn't been initialized
  while (toRead > append_sz)
  {
    fla_object_read(flanh->fs, flanh->ph, flaobj, rb, *rb_off + FLAN_APPEND_SIZE, FLAN_APPEND_SIZE);
    *rb_off += FLAN_APPEND_SIZE;
    memcpy(bufpos, rb, FLAN_APPEND_SIZE);
    toRead -= FLAN_APPEND_SIZE;
    bufpos += FLAN_APPEND_SIZE;
    offset += FLAN_APPEND_SIZE;
  }

  fla_object_read(flanh->fs, flanh->ph, flaobj, rb, *rb_off + FLAN_APPEND_SIZE, FLAN_APPEND_SIZE);
  *rb_off += FLAN_APPEND_SIZE;
  memcpy(bufpos, rb + offset % FLAN_APPEND_SIZE, toRead);

  return len;

}

ssize_t flan_object_read_rw(uint64_t oh, void *buf, size_t offset, size_t len,
                              struct flan_handle *flanh, struct flan_oinfo *oinfo)
{
  struct fla_object *nfsobj = &oinfo->fla_oh;
  uint32_t bs = flanh->append_sz;
  int ret;

  // Cant read past end of file
  if (offset + len > oinfo->size)
    len = oinfo->size - offset;

  // Aligned start and end go straight to the underlying storage if buffer is aligned
  if (!(offset % bs) && !(len % bs))
  {
    if (!((uintptr_t)buf % bs))
    {
      ret =  fla_object_read(flanh->fs, flanh->ph, nfsobj, buf, offset, len);
      if (ret)
        return ret;

      return len;
    }
  }

  ret = flan_object_unaligned_read(oh, nfsobj, buf, offset, len, flanh);
  if (ret)
    return ret;

  return len;
}

ssize_t flan_object_read(uint64_t oh, void *buf, size_t offset, size_t len,
                           struct flan_handle *flanh)
{
  struct flan_oinfo *oinfo = &flan_otable[oh].oinfo;
  // Cant read past end of file
  if (offset + len > oinfo->size)
    len = oinfo->size - offset;

  if (flan_otable[oh].o_flags & FLAN_OPEN_FLAG_WRITE)
    return flan_object_read_rw(oh, buf, offset, len, flanh, oinfo);

  return flan_object_read_r(oh, buf, offset, len, flanh, oinfo);
}
int flan_update_md(struct flan_ohandle *oh, struct flan_handle *flanh)
{
  int ret = 0;
  struct fla_object *nfsobj = &oh->flan_oh;
  char *tbuf = fla_buf_alloc(flanh->fs, flan_obj_sz); // On disk MD buf
  struct flan_oinfo *oinfo;

  ret = fla_object_read(flanh->fs, flanh->ph, nfsobj, tbuf, 0, flan_obj_sz);
  if (ret)
  {
    printf("Object read in update md fails\n");
    free(tbuf);
    return -EIO;
  }

  oinfo = ((struct flan_oinfo *)tbuf) + oh->oinfo_off;
  memcpy(oinfo, &oh->oinfo, sizeof(struct flan_oinfo));
  ret = fla_object_write(flanh->fs, flanh->ph, nfsobj, tbuf, 0, flan_obj_sz);
  if (ret)
  {
    printf("Object write in update md fails\n");
    free(tbuf);
    return -EIO;
  }

  free(tbuf);
  return ret;
}



int flan_conv_object_write(struct fla_object *nfs_oh, void *buf, size_t offset,
    size_t len, struct flan_handle *flanh)
{
  int ret;

  if (len % flan_dev_bs(flanh))
    ret = fla_object_unaligned_write(flanh->fs, flanh->ph, nfs_oh, buf, offset, len);
  else
    ret = fla_object_write(flanh->fs, flanh->ph, nfs_oh, buf, offset, len);

  return ret;
}

// This is currently only going to support append, force this later
int flan_zns_object_write(struct fla_object *nfs_oh, void *buf, size_t offset,
    size_t len, uint64_t oh, struct flan_handle *flanh)
{
  size_t al_start = 0, al_end = 0, al_len = 0, tail_len = 0;
  int ret = 0;
  char *bufpos = (char *)buf;
  char *al_buf;
  uint32_t bs = flanh->append_sz;

  // Unaligned start offset
  if (offset % bs)
    al_start = offset + (bs - (offset % bs));
  else
    al_start = offset;

  // Unaligned tail
  if ((offset + len) % bs)
    al_end = ((offset + len) / bs) * bs;
  else
      al_end = offset + len;

  al_len = al_end - al_start;
  tail_len = len - ((al_start - offset) + al_len);

  // We fit into the buffer so just fill buffer up
  if (offset + len < al_start)
  {
    memcpy(flan_otable[oh].append_buf + (offset % bs), buf, len);
    flan_otable[oh].append_off += len;
    return ret;
  }

  size_t buf_offset = (offset / bs) * bs;
  // Going to cross block boundary, copy up to the block boundary
  if (al_start != offset)
  {
    memcpy(flan_otable[oh].append_buf + (offset % bs), buf, al_start - offset);
    ret = fla_object_write(flanh->fs, flanh->ph, nfs_oh, flan_otable[oh].append_buf, buf_offset,
                             bs);
    if (ret)
    {
      printf("Error writing append buffer\n");
      return ret;
    }
  }
  // Write out all of the aligned data
  bufpos += al_start - offset;
  if (al_len) {
    al_buf = flan_buf_alloc(al_len, flanh);
    memcpy(al_buf, bufpos, al_len);
	  ret = fla_object_write(flanh->fs, flanh->ph, nfs_oh, al_buf, al_start, al_len);
    free(al_buf);
  }

  if (ret)
  {
    printf("ZNS write of the aligned portion of append data fails\n");
    return ret;
  }

  bufpos += al_len;
  // Copy the unaligned tail to the buffer
  memcpy(flan_otable[oh].append_buf, bufpos, tail_len);
  flan_otable[oh].append_off += len;

  return ret;
}

int flan_object_write(uint64_t oh, void *buf, size_t offset, size_t len,
                        struct flan_handle *flanh)
{
  struct flan_oinfo *oinfo = &flan_otable[oh].oinfo;
  struct fla_object *nfsobj = &oinfo->fla_oh;
  int ret = 0;

  if (flanh->is_zns)
    ret = flan_zns_object_write(nfsobj, buf, offset, len, oh, flanh);
  else
    ret = flan_conv_object_write(nfsobj, buf, offset, len, flanh);

  if (ret)
  {
    printf("flan_object_write fla object write fails\n");
    return -EIO;
  }

  if (offset + len > oinfo->size)
  {
    oinfo->size = offset + len;

    if (!flanh->is_zns)
    {
      ret = flan_update_md(&flan_otable[oh], flanh);
      if (ret)
        exit(0);
    }
  }

  return ret;
}

void *flan_buf_alloc(size_t nbytes, struct flan_handle *flanh)
{
  return fla_buf_alloc(flanh->fs, nbytes);
}

uint32_t flan_dev_bs(struct flan_handle *flanh)
{
  return fla_fs_lb_nbytes(flanh->fs);
}

// TODO update to reflect object table
int flan_object_rename(const char *oldname, const char *newname, struct flan_handle *flanh)
{
  struct flan_oinfo *oinfo = flan_find_oinfo(flanh, basename((char *)oldname));
  unsigned int namelen = strlen(basename((char *)newname));
  uint64_t oh;

  oh = flan_otable_search(basename((char *)oldname), NULL);
  flan_object_delete(basename((char *)newname), flanh); // TODO clear out even if we have the file currently open

  if (!oinfo)
  {
    return -EINVAL;
  }

  if (oh != FLAN_MAX_OPEN_OBJECTS) {
    memset(flan_otable[oh].oinfo.name, 0, FLAN_OBJ_NAME_LEN_MAX);  
    memcpy(flan_otable[oh].oinfo.name, basename((char *)newname), namelen + 1);
  }
  
  memset(oinfo->name, 0, FLAN_OBJ_NAME_LEN_MAX);
  memcpy(oinfo->name, basename((char *)newname), namelen + 1);

  if (!flanh->is_zns)
  {
    if (fla_object_write(flanh->fs, flanh->ph, &flan_dir.fla_oh, flan_dir.buf, 0, flan_obj_sz))
    {
      printf("Error writing md during rename for object:%s\n", oinfo->name);
      exit(0);
    }
  }

  return 0;
}

// TODO assumes this won't fail, revisit me
int flan_object_close(uint64_t oh, struct flan_handle *flanh)
{
  struct fla_object *noh = &flan_otable[oh].oinfo.fla_oh;
  size_t append_off = flan_otable[oh].append_off;
  uint32_t bs = flanh->append_sz;
  int ret = 0;

  // File is no longer open
  if (!strlen(flan_otable[oh].oinfo.name))
    return 0;

  flan_otable[oh].use_count--;
  // Use count drops to zero lets free
  if (!flan_otable[oh].use_count)
  {
    if (append_off % bs && !flan_otable[oh].frozen)
    {
      // Append the last block
      ret = fla_object_write(flanh->fs, flanh->ph, noh, flan_otable[oh].append_buf,
                             (append_off / bs) * bs, bs);
      if (ret)
      {
        printf("Error writing last block, corruption abound\n");
        return ret;
      }
    }

    // Seal the object
    if (append_off && flanh->is_zns)
      fla_object_seal(flanh->fs, flanh->ph, noh);

    // TODO fix this issue
    if (flan_otable[oh].flan_oh.slab_id != flan_dir.fla_oh.slab_id &&
		    flan_otable[oh].flan_oh.entry_ndx != flan_dir.fla_oh.entry_ndx)
    {
      printf("MD will be inconsistent\n");
    }
    else
    {
      memcpy(((struct flan_oinfo *)flan_dir.buf) + flan_otable[oh].oinfo_off, &flan_otable[oh].oinfo,
          sizeof(struct flan_oinfo));
    }

    free(flan_otable[oh].append_buf);
    memset(&flan_otable[oh], 0, sizeof(struct flan_ohandle));

  }

  return 0;
}

int flan_sync(struct flan_handle *flanh)
{
  return fla_sync(flanh->fs);
}

bool flan_is_zns(struct flan_handle *flanh)
{
  return flanh->is_zns;
}

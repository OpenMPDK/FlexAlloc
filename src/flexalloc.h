/**
 * flexalloc disk structures.
 *
 * Copyright (C) 2021 Jesper Devantier <j.devantier@samsung.com>
 * Copyright (C) 2021 Joel Granados <j.granados@samsung.com>
 * Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>
 *
 * @file flexalloc.h
 */
#ifndef __FLEXALLOC_H_
#define __FLEXALLOC_H_
#include <stdint.h>
#include <libxnvme.h>
#include "flexalloc_shared.h"
#include "flexalloc_freelist.h"
#include "flexalloc_hash.h"

/// flexalloc device handle
struct fla_dev
{
  struct xnvme_dev *dev;
  char * dev_uri;
  struct xnvme_dev *md_dev;
  char * md_dev_uri;
  uint32_t lb_nbytes;
};

struct fla_slab_flist_cache
{
  /// flexalloc system handle
  struct flexalloc *_fs;
  /// Head of cache array, entry at offset n corresponds to slab with id n
  struct fla_slab_flist_cache_elem *_head;
};

struct fla_geo_pool_sgmt
{
  /// number of logical blocks for the freelist
  uint32_t freelist_nlb;
  /// number of logical blocks to contain the hash table
  uint32_t htbl_nlb;
  /// number of slots in the hash table
  uint32_t htbl_tbl_size;
  /// number of logical blocks to contain the pool entries
  uint32_t entries_nlb;
};

struct fla_geo_slab_sgmt
{
  // # LBs for all slab segment
  uint32_t slab_sgmt_nlb;
};

/// Describes flexalloc disk geometry
struct fla_geo
{
  /// number of LBAs / disk size
  uint64_t nlb;
  /// LBA width, in bytes, read from disk
  uint32_t lb_nbytes;
  /// size of slabs in number of LBAs
  uint32_t slab_nlb;
  /// number of pools to reserve space for
  uint32_t npools;
  /// number of slabs
  uint32_t nslabs;

  /// number of blocks for fixed portion of metadata
  uint32_t md_nlb;
  /// blocks needed for pool segment
  struct fla_geo_pool_sgmt pool_sgmt;
  /// blocks needed for slab segment
  struct fla_geo_slab_sgmt slab_sgmt;
  /// Number of zones
  uint32_t nzones;
  /// Number of sectors in zone
  uint64_t nzsect;
  /// Controls flexalloc zns logic
  enum xnvme_geo_type type;
};

struct fla_pools
{
  /// pointer to pools freelist bit array
  freelist_t freelist;
  /// htbl data structure
  struct fla_htbl htbl;
  /// buffer into which to update the htbl data
  struct fla_pool_htbl_header *htbl_hdr_buffer;
  /// array of pool entries
  struct fla_pool_entry *entries;
};

struct fla_slabs
{
  struct fla_slab_header *headers;

  uint32_t *fslab_num;
  uint32_t *fslab_head;
  uint32_t *fslab_tail;
};

/// flexalloc handle
struct flexalloc
{
  struct fla_dev dev;
  unsigned int state;
  /// buffer holding all the disk-wide flexalloc metadata
  ///
  /// NOTE: allocated as an IO buffer.
  void *fs_buffer;

  struct fla_slab_flist_cache slab_cache;

  struct fla_geo geo;

  struct fla_super *super;
  struct fla_pools pools;
  struct fla_slabs slabs;

  struct fla_fns fns;

  /// pointer for the application to associate additional data
  void *user_data;
};

#endif // __FLEXALLOC_H_

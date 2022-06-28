// Copyright (C) 2021 Jesper Devantier <j.devantier@samsung.com>
// Copyright (C) 2021 Joel Granados <j.granados@samsung.com>
// Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>
#include <asm-generic/errno-base.h>
#include <libxnvme.h>
#include <libxnvmec.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "flexalloc.h"
#include "flexalloc_util.h"
#include "flexalloc_hash.h"
#include "flexalloc_freelist.h"
#include "flexalloc_slabcache.h"
#include "flexalloc_xnvme_env.h"
#include "flexalloc_mm.h"
#include "flexalloc_util.h"
#include "flexalloc_ll.h"
#include "flexalloc_slabcache.h"
#include "flexalloc_shared.h"
#include "flexalloc_znd.h"

static inline bool
fla_geo_zoned(const struct fla_geo *geo)
{
  return geo->type == XNVME_GEO_ZONED;
}

static int
fla_dev_sanity_check(struct xnvme_dev const * dev, struct xnvme_dev const *md_dev)
{
  int err = 0;
  uint32_t mdts_nbytes;

  /*
   * xNVMe's linux backend can potentially fallback on an incorrect minimum data transfer
   * (mdts) value if application is not executed with admin privileges. Check here that we
   * have an mdts of FLA_MDTS_MIN_NBYTES bytes.
   */
  mdts_nbytes = fla_xne_dev_mdts_nbytes(dev);
  err |= mdts_nbytes <= FLA_MDTS_MIN_NBYTES;
  if (md_dev)
  {
    mdts_nbytes = fla_xne_dev_mdts_nbytes(md_dev);
    err |= mdts_nbytes <= FLA_MDTS_MIN_NBYTES;
  }
  FLA_ERR(err, "The minimum data transfer value of dev or md_dev reported to be less " \
          "than %d. This is most probably due to lack of administrative privileges. "\
          " you can solve this by running with sudo for example.", FLA_MDTS_MIN_NBYTES);

  if (md_dev)
  {
    err |= fla_xne_dev_lba_nbytes(md_dev) != fla_xne_dev_lba_nbytes(dev);
    FLA_ERR(err, "MD DEV LBA size != DEV LBA SIZE");
  }

  return err;
}

void
print_slab_sgmt(const struct flexalloc * fs)
{
  struct fla_slab_header * slab;

  fprintf(stderr, "%s\n", "--------------------------------");
  fprintf(stderr, "Slabs number : %d\n", *fs->slabs.fslab_num);
  fprintf(stderr, "Slabs head : %d\n", *fs->slabs.fslab_head);
  fprintf(stderr, "Slabs tail : %d\n", *fs->slabs.fslab_tail);
  fprintf(stderr, "Header ptr : %p\n", fs->slabs.headers);

  /*for(int i = 0 ; i < 1024 ; ++i)
  {
    fprintf(stderr, "%0x", (char)*((char*)fs->slabs.headers + i));
  }
  fprintf(stderr, "\n");*/

  for(uint32_t i = 0 ; i < fs->geo.nslabs ; ++i)
  {
    slab = fs->slabs.headers + i;
    fprintf(stderr, "slab number %d\n", i);
    fprintf(stderr, "next : %d\n", slab->next);
    fprintf(stderr, "prev : %d\n", slab->prev);
  }
  fprintf(stderr, "%s\n", "--------------------------------");

}

uint32_t
fla_slab_sgmt_calc(uint32_t nslabs, uint32_t lb_nbytes)
{
  /*
   * The int32_t free count, head and tail IDs are located at the last 96 bits of the slab_sgmt.
   * Leave room after the slab headers for whatever comes at the end.
   */
  const size_t needed_end_nbytes = sizeof(uint32_t)*3;
  const size_t slab_headers_nbytes = nslabs * sizeof(struct fla_slab_header);

  return FLA_CEIL_DIV(slab_headers_nbytes + needed_end_nbytes, lb_nbytes);
}

void
fla_geo_slab_sgmt_calc(uint32_t nslabs, uint32_t lb_nbytes,
                       struct fla_geo_slab_sgmt *geo)
{
  geo->slab_sgmt_nlb = fla_slab_sgmt_calc(nslabs, lb_nbytes);
}

static inline uint32_t
fla_geo_slab_sgmt_nblocks(const struct fla_geo_slab_sgmt *geo)
{
  return geo->slab_sgmt_nlb;
}

static inline uint32_t
fla_nslabs_max_mddev(uint64_t blocks, uint32_t slab_nlb, uint32_t lb_nbytes,
                     uint64_t md_blocks)
{
  uint32_t nslabs = blocks / slab_nlb;
  uint32_t md_nlb = fla_slab_sgmt_calc(nslabs, lb_nbytes);

  /* Make sure the meta data device has enough room to store slab MD */
  if (md_nlb <= md_blocks)
    return nslabs;
  else
    return 0;
}

uint32_t
fla_nslabs_max(uint64_t blocks, uint32_t slab_nlb, uint32_t lb_nbytes)
{
  /*
   * Given 'blocks', distribute them such that we get the most number of slabs
   * while retaining enough space to house the accompanying metadata.
   */
  uint32_t nslabs = blocks / slab_nlb;
  uint32_t md_nlb;
  while (nslabs)
  {
    md_nlb = fla_slab_sgmt_calc(nslabs, lb_nbytes);
    if (blocks - nslabs * slab_nlb >= md_nlb)
    {
      break;
    }
    nslabs--;
  }
  return nslabs;
}

void
fla_geo_pool_sgmt_calc(uint32_t npools, uint32_t lb_nbytes,
                       struct fla_geo_pool_sgmt *geo)
{
  /*
   * Infer the number of blocks used for the various parts of the pool segment
   * provided the number of pools to support and the logical block size.
   *
   * The pool segment is laid out in the following sections:
   * * freelist, 1 bit per pool, packed
   * * htbl - ~8B header and 2 16B entries per pool, packed
   * * entries - 1 ~144B entry per pool, packed
   *
   * Each section is rounded up to a multiple of logical blocks.
   */
  geo->freelist_nlb = FLA_CEIL_DIV(fla_flist_size(npools), lb_nbytes);
  /*
   * Allocate 2 table entries per intended entry
   *
   * Hash table algorithms degrade as the table is filled as the further the
   * hash- and compression functions deviate from the ideal of perfect distribution, the
   * more collisions will be encountered.
   * Over-provisioning by a factor of 3/2-2 is a common practice to alleviate this.
   */
  geo->htbl_tbl_size = npools * 2;
  geo->htbl_nlb = FLA_CEIL_DIV(
                    sizeof(struct fla_pool_htbl_header)
                    + geo->htbl_tbl_size * sizeof(struct fla_htbl_entry), lb_nbytes);
  geo->entries_nlb = FLA_CEIL_DIV(npools * sizeof(struct fla_pool_entry), lb_nbytes);
}

/// total number of blocks in pool segment
static inline uint32_t
fla_geo_pool_sgmt_nblocks(const struct fla_geo_pool_sgmt *geo)
{
  return geo->freelist_nlb
         + geo->htbl_nlb
         + geo->entries_nlb;
}

void
fla_geo_from_super(struct xnvme_dev *dev, struct fla_super *super,
                   struct fla_geo *geo)
{
  /*
   * Re-caclculate geometry based on provided input parameters
   *
   * This routine re-calculates disk geometry based on the npools and nslabs
   * parameters which are read from flexalloc' super block.
   *
   */

  //...
  geo->lb_nbytes = fla_xne_dev_lba_nbytes(dev);
  geo->nlb = fla_xne_dev_tbytes(dev) / geo->lb_nbytes;
  geo->nzones = fla_xne_dev_znd_zones(dev);
  geo->nzsect = fla_xne_dev_znd_sect(dev);
  geo->type = fla_xne_dev_type(dev);

  geo->slab_nlb = super->slab_nlb;
  geo->npools = super->npools;
  geo->nslabs = super->nslabs;
  geo->md_nlb = super->md_nlb;
  fla_geo_pool_sgmt_calc(geo->npools, geo->lb_nbytes, &geo->pool_sgmt);
  fla_geo_slab_sgmt_calc(geo->nslabs, geo->lb_nbytes, &geo->slab_sgmt);
}

uint32_t
fla_geo_nblocks(const struct fla_geo *geo)
{
  return geo->md_nlb
         + fla_geo_pool_sgmt_nblocks(&geo->pool_sgmt)
         + fla_geo_slab_sgmt_nblocks(&geo->slab_sgmt);
}

uint64_t
fla_geo_nbytes(struct fla_geo *geo)
{
  return ((uint64_t)fla_geo_nblocks(geo)) * geo->lb_nbytes;
}


void
fla_geo_init(const struct xnvme_dev *dev, uint32_t npools, uint32_t slab_nlb,
             uint32_t lb_nbytes, struct fla_geo *geo)
{
  if (!dev)
    return;

  geo->lb_nbytes = lb_nbytes;
  geo->nlb = fla_xne_dev_tbytes(dev) / geo->lb_nbytes;
  geo->nzones = fla_xne_dev_znd_zones(dev);
  geo->nzsect = fla_xne_dev_znd_sect(dev);
  geo->type = fla_xne_dev_type(dev);

  geo->npools = npools;
  geo->slab_nlb = slab_nlb;

  geo->md_nlb = FLA_CEIL_DIV(sizeof(struct fla_super), lb_nbytes);
}

int
fla_mkfs_geo_calc(const struct xnvme_dev *dev, const struct xnvme_dev *md_dev,
                  uint32_t npools, uint32_t slab_nlb, struct fla_geo *geo)
{
  /*
   * Calculate geometry of disk format given the SLAB size (in number of logical blocks)
   * and optionally how many pools to support.
   *
   * If the number of pools is not supplied, assume (roughly) 1 pool per slab. This is a
   * very high number of pools, but the overhead should be miniscule.
   *
   * NOTE: this procedure is intended during creation of the system. When opening an existing
   *       system, read the super block and call the fla_geo_init procedure.
   */
  uint32_t nslabs_approx;
  uint32_t lb_nbytes = fla_xne_dev_lba_nbytes(dev);
  struct fla_geo md_geo;

  fla_geo_init(dev, npools, slab_nlb, lb_nbytes, geo);
  fla_geo_init(dev, npools, slab_nlb, lb_nbytes, &md_geo);

  if (fla_geo_zoned(geo) && geo->slab_nlb % geo->nzsect)
  {
    FLA_ERR_PRINTF("Znd dev slb sz:%u not multiple of zone sz:%lu\n", slab_nlb, geo->nzsect);
    return -1;
  }
  // estimate how many slabs we can get before knowing the overhead of the pool metadata
  if (!md_dev)
  {
    nslabs_approx = fla_nslabs_max(geo->nlb - geo->md_nlb, slab_nlb, lb_nbytes);
  }
  else
  {
    nslabs_approx = fla_nslabs_max_mddev(geo->nlb, slab_nlb, lb_nbytes, md_geo.nlb);
  }

  if (FLA_ERR(!nslabs_approx, "slab size too large - not enough space to allocate any slabs"))
  {
    return -1;
  }

  if (FLA_ERR(npools > nslabs_approx, "npools is too high"))
  {
    return -1;
  }
  else if (!npools)
  {
    // no desired number of pools specified, assume a max of 1 pool per slab (worst-case)
    geo->npools = nslabs_approx;
  }

  // calculate number of blocks required to support the pool entries
  fla_geo_pool_sgmt_calc(geo->npools, lb_nbytes, &geo->pool_sgmt);

  // calculate maximum number of slabs we can support given the logical blocks remaining
  if (!md_dev)
  {
    geo->nslabs = fla_nslabs_max(
                    geo->nlb - geo->md_nlb - fla_geo_pool_sgmt_nblocks(&geo->pool_sgmt),
                    slab_nlb,
                    lb_nbytes);
  }
  else
  {
    geo->nslabs = fla_nslabs_max_mddev(geo->nlb, slab_nlb, lb_nbytes,
                                       md_geo.nlb - geo->md_nlb -
                                       fla_geo_pool_sgmt_nblocks(&geo->pool_sgmt));
  }

  if (FLA_ERR(!geo->nslabs, "slab size too large, not enough space to allocate any slabs"))
  {
    return -1;
  }

  // calculate number of blocks required to support `nslabs` slab header entries
  fla_geo_slab_sgmt_calc(geo->nslabs, lb_nbytes, &geo->slab_sgmt);

  if (FLA_ERR(npools > geo->nslabs,
              "Each pool will require 1+ slabs. Having more pools than slabs is invalid"))
  {
    return -1;
  }
  else if (geo->npools > geo->nslabs)
  {
    // if npools was not specified, yet we inferred more pools than we can allocate
    // slabs for, lower the number of pools accordingly.
    geo->npools = geo->nslabs;
    fla_geo_pool_sgmt_calc(geo->npools, lb_nbytes, &geo->pool_sgmt);
  }

  return 0;
}

/// calculate disk offset, in logical blocks, of the start of the pools segment
uint64_t
fla_geo_pool_sgmt_lb_off(struct fla_geo const *geo)
{
  return geo->md_nlb;
}

/// calculate disk offset in bytes of the start of the pools segment
uint64_t
fla_geo_pool_sgmt_off(struct fla_geo *geo)
{
  return geo->lb_nbytes * fla_geo_pool_sgmt_lb_off(geo);
}

/// calculate disk offset, in logical blocks, of the start of the slabs segment
uint64_t
fla_geo_slab_sgmt_lb_off(struct fla_geo const *geo)
{
  return fla_geo_pool_sgmt_lb_off(geo)
         + fla_geo_pool_sgmt_nblocks(&geo->pool_sgmt);
}

uint64_t
fla_geo_slabs_lb_off(struct fla_geo const *geo)
{
  return fla_geo_slab_sgmt_lb_off(geo) + fla_geo_slab_sgmt_nblocks(&geo->slab_sgmt);
}

uint64_t
fla_geo_slab_lb_off(struct flexalloc const *fs, uint32_t slab_id)
{
  uint64_t slabs_base = 0, slab_base;

  if (fs->dev.md_dev == NULL)
    slabs_base = fla_geo_slabs_lb_off(&fs->geo);

  slab_base = slabs_base + (slab_id * fs->geo.slab_nlb);
  if (fla_geo_zoned(&fs->geo) && slab_base % fs->geo.nzsect)
  {
    slab_base += (slab_base % fs->geo.nzsect);
  }

  return slab_base;
}

/// calculate disk offset in bytes of the start of the slabs segment
uint64_t
fla_geo_slab_sgmt_off(struct fla_geo *geo)
{
  return geo->lb_nbytes * fla_geo_slab_sgmt_lb_off(geo);
}

void
fla_mkfs_slab_sgmt_init(struct flexalloc *fs, struct fla_geo *geo)
{
  const uint64_t slab_sgmt_nbytes = geo->lb_nbytes * ((uint64_t)geo->slab_sgmt.slab_sgmt_nlb);
  const uint32_t last_slab_id = geo->nslabs - 1;
  const uint32_t first_slab_id = 0;
  struct fla_slab_header * curr_slab;

  memset(fs->slabs.headers, 0, slab_sgmt_nbytes);

  // tail points to the last 32 bits in slab_sgmt and is set to highest possible slab id
  fs->slabs.fslab_tail = (uint32_t*)((unsigned char*)fs->slabs.headers + slab_sgmt_nbytes - sizeof(
                                       uint32_t));
  *fs->slabs.fslab_tail = last_slab_id;

  // head points to the penultimate 32 bits in slab_sgmt and is set to lowest possible id
  fs->slabs.fslab_head = (uint32_t*)((unsigned char*)fs->slabs.headers + slab_sgmt_nbytes - (sizeof(
                                       uint32_t)*2));
  *fs->slabs.fslab_head = first_slab_id;

  fs->slabs.fslab_num = (uint32_t*)((unsigned char*)fs->slabs.headers + slab_sgmt_nbytes - (sizeof(
                                      uint32_t)*3));
  *fs->slabs.fslab_num = geo->nslabs;

  // Set first slab header
  curr_slab = fs->slabs.headers;
  curr_slab->next = first_slab_id + 1;
  curr_slab->prev = FLA_LINKED_LIST_NULL;

  //set last slab header
  curr_slab = fs->slabs.headers + last_slab_id;
  curr_slab->next = FLA_LINKED_LIST_NULL;
  curr_slab->prev = last_slab_id - 1;

  for(uint32_t curr_slab_id = 1 ; curr_slab_id < last_slab_id ; ++curr_slab_id)
  {
    curr_slab = fs->slabs.headers + curr_slab_id;
    curr_slab->next = curr_slab_id + 1;
    curr_slab->prev = curr_slab_id - 1;
  }

}

void
fla_mkfs_pool_sgmt_init(struct flexalloc *fs, struct fla_geo *geo)
{
  // initialize freelist.
  fla_flist_init(fs->pools.freelist, geo->npools);

  // initialize hash table header
  // this stores the size of the table and the number of elements
  fs->pools.htbl_hdr_buffer->len = 0;
  fs->pools.htbl_hdr_buffer->size = geo->pool_sgmt.htbl_tbl_size;

  // initialize hash table entries
  fla_htbl_entries_init(fs->pools.htbl.tbl, fs->pools.htbl_hdr_buffer->size);

  // initialize the entries themselves
  memset(fs->pools.entries, 0, (geo->lb_nbytes * geo->pool_sgmt.entries_nlb));
}

void
fla_mkfs_super_init(struct flexalloc *fs, struct fla_geo *geo)
{
  fs->super->magic = FLA_MAGIC;
  fs->super->fmt_version = FLA_FMT_VER;
  fs->super->md_nlb = geo->md_nlb;
  fs->super->npools = geo->npools;
  fs->super->slab_nlb = geo->slab_nlb;
  fs->super->nslabs = geo->nslabs;
}

int
fla_init(struct fla_geo *geo, struct xnvme_dev *dev, struct xnvme_dev *md_dev, void *fla_md_buf,
         struct flexalloc *fs)
{
  /*
   * Initializes flexalloc struct from knowledge of the disk geometry, and a buffer containing
   * the complete range of LB's containing the on-disk meta data.
   *
   * NOTE: flexalloc will be backed by data in the fla_md_buf buffer. This buffer should
   *       be an IO buffer such that it can be written to disk without having to copy
   *       the data.
   */
  uint8_t *buf_base = fla_md_buf;
  uint8_t *pool_sgmt_base;

  fs->dev.dev = dev;
  fs->dev.md_dev = md_dev;
  fs->dev.lb_nbytes = fla_xne_dev_lba_nbytes(dev);

  fs->fs_buffer = fla_md_buf;
  fs->geo = *geo;

  // super block is at the head of the buffer
  fs->super = fla_md_buf;

  /*
   * Pool segment
   */
  pool_sgmt_base = buf_base + fla_geo_pool_sgmt_off(geo);
  fs->pools.freelist = (freelist_t)(pool_sgmt_base);
  fs->pools.htbl_hdr_buffer = (struct fla_pool_htbl_header *)
                              (pool_sgmt_base
                               + (geo->lb_nbytes * geo->pool_sgmt.freelist_nlb));

  fs->pools.htbl.len = fs->pools.htbl_hdr_buffer->len;
  fs->pools.htbl.tbl_size = fs->pools.htbl_hdr_buffer->size;

  fs->pools.htbl.tbl = (struct fla_htbl_entry *)(fs->pools.htbl_hdr_buffer + 1);

  fs->pools.entries =(struct fla_pool_entry *)
                     (pool_sgmt_base
                      + (geo->lb_nbytes
                         * (geo->pool_sgmt.freelist_nlb
                            + geo->pool_sgmt.htbl_nlb)));
  /*
   * Slab segment
   */

  fs->slabs.headers = (struct fla_slab_header *) (buf_base + fla_geo_slab_sgmt_off(geo));
  fs->slabs.fslab_tail = (uint32_t*)((unsigned char *)fs->slabs.headers +
                                     (geo->slab_sgmt.slab_sgmt_nlb * geo->lb_nbytes) - sizeof(uint32_t));
  fs->slabs.fslab_head = (uint32_t*)((unsigned char*)fs->slabs.headers +
                                     (geo->slab_sgmt.slab_sgmt_nlb * geo->lb_nbytes) - (sizeof(uint32_t)*2));
  fs->slabs.fslab_num = (uint32_t*)((unsigned char*)fs->slabs.headers +
                                    (geo->slab_sgmt.slab_sgmt_nlb * geo->lb_nbytes) - (sizeof(uint32_t)*3));

  return 0;
}

int
fla_mkfs(struct fla_mkfs_p *p)
{
  struct xnvme_dev *dev = NULL, *md_dev = NULL;
  struct flexalloc *fs;
  struct fla_geo geo;
  void *fla_md_buf;
  size_t fla_md_buf_len;
  int err;

  err = fla_xne_dev_open(p->dev_uri, NULL, &dev);
  if (FLA_ERR(err, "failed to open device"))
    return -1;

  err = fla_xne_dev_mkfs_prepare(dev, p->md_dev_uri, &md_dev);
  if (FLA_ERR(err, "fla_xne_dev_mkfs_prepare"))
    return err;

  err = fla_dev_sanity_check(dev, md_dev);
  if(FLA_ERR(err, "fla_dev_sanity_check()"))
    goto exit;


  if (md_dev)
  {
    err |= fla_xne_dev_lba_nbytes(md_dev) != fla_xne_dev_lba_nbytes(dev);
    if (err)
    {
      FLA_ERR(err, "MD DEV LBA size != DEV LBA SIZE");
      goto exit;
    }
  }

  err = fla_mkfs_geo_calc(dev, md_dev, p->npools, p->slab_nlb, &geo);
  if(FLA_ERR(err, "fla_mkfs_geo_calc()"))
    goto exit;

  if (!md_dev)
    md_dev = dev;

  fs = malloc(sizeof(struct flexalloc));
  if (FLA_ERR(!fs, "malloc()"))
  {
    err = -ENOMEM;
    goto exit;
  }

  memset(fs, 0, sizeof(struct flexalloc));
  fla_md_buf_len = fla_geo_nbytes(&geo);
  fla_md_buf = fla_xne_alloc_buf(md_dev, fla_md_buf_len);
  if (FLA_ERR(!fla_md_buf, "fla_xne_alloc_buf()"))
  {
    err = -ENOMEM;
    goto free_fs;
  }

  memset(fla_md_buf, 0, fla_md_buf_len);
  err = fla_init(&geo, dev, NULL, fla_md_buf, fs);
  if (FLA_ERR(err, "fla_init()"))
    goto free_md;

  // initialize pool
  fla_mkfs_pool_sgmt_init(fs, &geo);

  // initialize slab
  fla_mkfs_slab_sgmt_init(fs, &geo);

  // initialize super
  fla_mkfs_super_init(fs, &geo);

  err = fla_xne_sync_seq_w_nbytes(md_dev, FLA_SUPER_SLBA, fla_md_buf_len,
                                  fla_md_buf);
  if (FLA_ERR(err, "fla_xne_sync_seq_w_nbytes()"))
    goto free_md;


free_md:
  fla_xne_free_buf(md_dev, fla_md_buf);

free_fs:
  free(fs);

exit:
  if (md_dev && md_dev != dev)
    fla_xne_dev_close(md_dev);
  fla_xne_dev_close(dev);
  return err;
}

int
fla_super_read(struct xnvme_dev *dev, size_t lb_nbytes, struct fla_super **super)
{
  int err = 0;

  // allocate read buffer and compute the LBA range
  *super = fla_xne_alloc_buf(dev, lb_nbytes);
  if ((err = FLA_ERR(!*super, "fla_xne_alloc_buf()")))
  {
    err = -ENOMEM;
    goto exit;
  }

  memset(*super, 0, lb_nbytes); // Keep valgrind happy
  err = fla_xne_sync_seq_r_nbytes(dev, 0, lb_nbytes, *super);
  if (FLA_ERR(err, "fla_xne_sync_seq_r_nbytes()"))
    goto free_super;

  // TODO: Should this be placed in fla_dev_sanity_check
  if ((*super)->magic != FLA_MAGIC)
  {
    err = 22;
    FLA_ERR(err, "Invalid super block\n");
    goto free_super;
  }

  return 0;

free_super:
  fla_xne_free_buf(dev, *super);
exit:
  return err;
}

struct flexalloc *
fla_fs_alloc()
{
  struct flexalloc *fs = malloc(sizeof(struct flexalloc));
  if (FLA_ERR(!fs, "fla_fs_alloc malloc()"))
    return NULL;

  memset(fs, 0, sizeof(struct flexalloc));

  return fs;
}

void
fla_fs_free(struct flexalloc *fs)
{
  if (fs)
    free(fs);
}

int
fla_flush(struct flexalloc *fs)
{
  int err = 0;
  struct xnvme_dev *md_dev = fs->dev.md_dev;

  if (!fs || !(fs->state & FLA_STATE_OPEN))
    return 0;

  if (!md_dev)
    md_dev = fs->dev.dev;

  err = fla_slab_cache_flush(&fs->slab_cache);
  if (FLA_ERR(err, "fla_slab_cache_flush() - failed to flush one or more slab freelists"))
    goto exit;

  // We have to copy over the pool hash table's metadata before flushing
  fs->pools.htbl_hdr_buffer->len = fs->pools.htbl.len;
  err = fla_xne_sync_seq_w_naddrs(md_dev, FLA_SUPER_SLBA, fla_geo_nblocks(&fs->geo),
                                  fs->fs_buffer);
  if(FLA_ERR(err, "fla_xne_sync_seq_w_naddrs()"))
    goto exit;

exit:
  return err;
}

void
fla_close_noflush(struct flexalloc *fs)
{
  if (!fs || !(fs->state & FLA_STATE_OPEN))
    return;

  fs->state &= ~FLA_STATE_OPEN;
  fla_slab_cache_free(&fs->slab_cache);
  xnvme_dev_close(fs->dev.dev);
  free(fs->super);
  if (fs->dev.md_dev != fs->dev.dev)
    xnvme_dev_close(fs->dev.md_dev);
  free(fs->dev.dev_uri);
  if(fs->dev.md_dev_uri != NULL)
    free(fs->dev.md_dev_uri);
  free(fs);
}

int
fla_base_close(struct flexalloc *fs)
{
  int err = 0;

  if (!fs)
    return 0;

  err = fla_flush(fs);
  if(FLA_ERR(err, "fla_flush()"))
  {
    goto exit;
  }

  fla_close_noflush(fs);

exit:
  return err;
}

int
fla_base_sync(struct flexalloc *fs)
{
  return fla_flush(fs);
}

void
fla_print_geo(struct flexalloc *fs)
{
  struct fla_geo * geo = &(fs->geo);
  fprintf(stderr, "Flexalloc Geometry: \n");
  fprintf(stderr, "|  LBAs: %"PRIu64"\n", geo->nlb);
  fprintf(stderr, "|  LBA width: %"PRIu32"B\n", geo->lb_nbytes);
  fprintf(stderr, "|  md blocks: %"PRIu32"\n", geo->md_nlb);
  fprintf(stderr, "|  pool segment:\n");
  fprintf(stderr, "|    * pools: %"PRIu32"\n", geo->npools);
  fprintf(stderr, "|    * freelist blocks: %"PRIu32"\n", geo->pool_sgmt.freelist_nlb);
  fprintf(stderr, "|    * htbl blocks: %"PRIu32"\n", geo->pool_sgmt.htbl_nlb);
  fprintf(stderr, "|    * entry blocks: %"PRIu32"\n", geo->pool_sgmt.entries_nlb);
  fprintf(stderr, "|    * total blocks: %"PRIu32"\n", fla_geo_pool_sgmt_nblocks(&geo->pool_sgmt));
  fprintf(stderr, "|  slab segment:\n");
  fprintf(stderr, "|    * slabs: %"PRIu32"\n", geo->nslabs);
  fprintf(stderr, "|    * slab total blocks: %"PRIu32"\n", geo->slab_sgmt.slab_sgmt_nlb);
  fprintf(stderr, "\n");
}

void
fla_print_pool_entries(struct flexalloc *fs)
{
  int err = 0;
  struct fla_slab_header * curr_slab;
  uint32_t slab_heads[3];
  uint32_t tmp;
  struct fla_pool_entry * pool_entry;

  for (uint32_t npool = 0 ; npool < fs->geo.npools ; ++npool)
  {
    pool_entry = &fs->pools.entries[npool];

    slab_heads[0] = pool_entry->empty_slabs;
    slab_heads[1] = pool_entry->full_slabs;
    slab_heads[2] = pool_entry->partial_slabs;
    if(pool_entry->obj_nlb == 0 && pool_entry->slab_nobj == 0)
      continue; //as this pool has not been initialized

    fprintf(stderr, "Pool Entry %"PRIu32"(%p)\n", npool, pool_entry);
    fprintf(stderr, "|  obj_nlb : %"PRIu32"\n", pool_entry->obj_nlb);
    fprintf(stderr, "|  root_obj_hndl : %"PRIu64"\n", pool_entry->root_obj_hndl);
    fprintf(stderr, "|  strp_nobjs : %"PRIu32"\n", pool_entry->strp_nobjs);
    fprintf(stderr, "|  strp_nbytes : %"PRIu32"\n", pool_entry->strp_nbytes);
    fprintf(stderr, "|  PoolName : %s\n", pool_entry->name);
    fprintf(stderr, "|  Max Number of Objects In Slab %"PRIu32"\n", pool_entry->slab_nobj);
    fprintf(stderr, "|  Slabs:\n");
    for(size_t i = 0 ; i < 3 ; ++i)
    {
      fprintf(stderr, "|    Head : %d, offset %ld\n", slab_heads[i], i);
      tmp = slab_heads[i];
      for(uint32_t j = 0 ; j < fs->geo.nslabs && tmp != FLA_LINKED_LIST_NULL; ++j)
      {
        curr_slab = fla_slab_header_ptr(tmp, fs);
        if((err = FLA_ERR(!curr_slab, "fla_slab_header_ptr()")))
          return;

        fprintf(stderr, "|    -->next : %"PRIu32", prev : %"PRIu32", refcount : %"PRIu32"\n",
                curr_slab->next, curr_slab->prev, curr_slab->refcount);
        tmp = curr_slab->next;
      }
    }
  }
}

void
fla_print_fs(struct flexalloc *fs)
{
  fla_print_geo(fs);
  fla_print_pool_entries(fs);
}

int
fla_pool_release_all_slabs(struct flexalloc *fs, struct fla_pool_entry * pool_entry)
{
  int err = 0;
  uint32_t * heads[3] = {&pool_entry->empty_slabs, &pool_entry->full_slabs, &pool_entry->partial_slabs};
  for(size_t i = 0 ; i < 3 ; ++i)
  {
    err = fla_hdll_remove_all(fs, heads[i], fla_release_slab);
    if(FLA_ERR(err, "fla_hdll_remove_all()"))
    {
      goto exit;
    }
  }

exit:
  return err;
}

int
fla_base_pool_open(struct flexalloc *fs, const char *name, struct fla_pool **handle)
{
  struct fla_htbl_entry *htbl_entry;

  htbl_entry = htbl_lookup(&fs->pools.htbl, name);
  if (!htbl_entry) // TODO: Find error code for this valid error
    return -1;

  (*handle) = malloc(sizeof(struct fla_pool));
  if (FLA_ERR(!(*handle), "malloc()"))
    return -ENOMEM;

  (*handle)->h2 = htbl_entry->h2;
  (*handle)->ndx = htbl_entry->val;
  return 0;
}

void
fla_pool_entry_reset(struct fla_pool_entry *pool_entry, const char *name, int name_len,
                     uint32_t const obj_nlb, uint32_t const slab_nobj)
{
  memcpy(pool_entry->name, name, name_len);
  pool_entry->obj_nlb = obj_nlb;
  pool_entry->slab_nobj = slab_nobj;
  pool_entry->empty_slabs = FLA_LINKED_LIST_NULL;
  pool_entry->full_slabs = FLA_LINKED_LIST_NULL;
  pool_entry->partial_slabs = FLA_LINKED_LIST_NULL;
  pool_entry->root_obj_hndl = FLA_ROOT_OBJ_NONE;
  pool_entry->strp_nobjs = 1; // By default we don't stripe across objects
}

int
fla_base_pool_set_strp(struct flexalloc *fs, struct fla_pool *pool, uint32_t strp_nobjs,
                       uint32_t strp_nbytes)
{
  struct fla_pool_entry *pool_entry = &fs->pools.entries[pool->ndx];
  const struct xnvme_geo * geo = xnvme_dev_get_geo(fs->dev.dev);

  if (FLA_ERR(strp_nbytes > geo->mdts_nbytes, "Strp sz > mdts for device"))
    return -1;

  if (FLA_ERR(strp_nobjs > pool_entry->slab_nobj, "Strp sz > max obj in slab"))
    return -1;

  pool_entry->strp_nobjs = strp_nobjs;
  pool_entry->strp_nbytes = strp_nbytes;

  return 0;
}

uint32_t
fla_calc_objs_in_slab(struct flexalloc const * fs, uint32_t const obj_nlb)
{
  uint32_t obj_num;
  size_t unused_nlb, free_list_nlb;

  if (!fs->dev.md_dev)
  {
    for(obj_num = fs->geo.slab_nlb / obj_nlb ; obj_num > 0; --obj_num)
    {
      unused_nlb = (fs->geo.slab_nlb - (obj_num * obj_nlb));
      free_list_nlb = fla_slab_cache_flist_nlb(fs, obj_num);
      if(unused_nlb >= free_list_nlb)
        break;
    }
  }
  else   // For a zoned device we place free list nlb in the MD dev
    obj_num = fs->geo.slab_nlb / obj_nlb;

  return obj_num;
}

int
fla_base_pool_create(struct flexalloc *fs, const char *name, int name_len, uint32_t obj_nlb,
                     struct fla_pool **handle)
{
  int err;
  struct fla_pool_entry *pool_entry;
  int entry_ndx = 0;
  uint32_t slab_nobj;

  // Return pool if it exists
  err = fla_base_pool_open(fs, name, handle);
  if(!err)
  {
    pool_entry = &fs->pools.entries[(*handle)->ndx];
    if(pool_entry->obj_nlb != obj_nlb)
    {
      err = -EINVAL;
      goto free_handle;
    }
    return 0;
  }

  if ((err = FLA_ERR(name_len >= FLA_NAME_SIZE_POOL, "pool name too long")))
    goto exit;

  slab_nobj = fla_calc_objs_in_slab(fs, obj_nlb);
  if((err = FLA_ERR(slab_nobj < 1, "Object size is incompatible with slab size.")))
    goto exit;


  if (fla_geo_zoned(&fs->geo))
  {
    if ((err = FLA_ERR(obj_nlb != fs->geo.nzsect, "object sz != fomatted zone size")))
      goto exit;
  }

  (*handle) = malloc(sizeof(struct fla_pool));
  if (FLA_ERR(!(*handle), "malloc()"))
  {
    err = -ENOMEM;
    goto exit;
  }

  entry_ndx = fla_flist_entries_alloc(fs->pools.freelist, 1);
  if ((err = FLA_ERR(entry_ndx < 0, "failed to allocate pool entry")))
    goto free_handle;

  err = htbl_insert(&fs->pools.htbl, name, entry_ndx);
  if (FLA_ERR(err, "failed to create pool entry in hash table"))
    goto free_freelist_entry;

  pool_entry = &fs->pools.entries[entry_ndx];
  fla_pool_entry_reset(pool_entry, name, name_len, obj_nlb, slab_nobj);

  (*handle)->ndx = entry_ndx;
  (*handle)->h2 = FLA_HTBL_H2(name);

  return 0;

free_freelist_entry:
  fla_flist_entry_free(fs->pools.freelist, entry_ndx);

free_handle:
  free(*handle);

exit:
  return err;
}

int
fla_base_pool_destroy(struct flexalloc *fs, struct fla_pool * handle)
{
  struct fla_pool_entry *pool_entry;
  struct fla_htbl_entry *htbl_entry;
  int err = 0;
  if ((err = FLA_ERR(handle->ndx > fs->super->npools, "invalid pool id, out of range")))
    goto exit;

  pool_entry = &fs->pools.entries[handle->ndx];
  htbl_entry = htbl_lookup(&fs->pools.htbl, pool_entry->name);
  if ((err = FLA_ERR(!htbl_entry, "failed to find pool entry in hash table")))
    goto exit;

  /*
   * Name given by pool entry pointed to by handle->ndx resolves to a different
   * h2 (secondary hash) value than indicated by handle.
   * This means the handle is stale/invalid - the entry is either unused or used
   * for some other entry.
   */
  if ((err = FLA_ERR(htbl_entry->h2 != handle->h2,
                     "stale/invalid pool handle - resolved to an unused/differently named pool")))
    goto exit;

  if ((err = FLA_ERR(htbl_entry->val != handle->ndx,
                     "stale/invalid pool handle - corresponding hash table entry points elsewhere")))
    goto exit;


  err = fla_pool_release_all_slabs(fs, pool_entry);
  if(FLA_ERR(err, "fla_pool_release_all_slabs()"))
    goto exit;

  err = fla_flist_entry_free(fs->pools.freelist, handle->ndx);
  if (FLA_ERR(err,
              "could not clear pool freelist entry - probably inconsistency in the metadat"))
    /*
     * We would normally undo our actions to abort cleanly.
     * However, this should only happen in case the pool_id given is within
     * the range of pools as specified in the super block, but that the freelist
     * somehow has fewer entries anyway (inconsistency).
     */
    goto exit;

  // remove hash table entry, note the freelist entry is the canonical entry.
  htbl_remove(&fs->pools.htbl, pool_entry->name);

  free(handle);

  // TODO: release slabs controlled by pool

exit:
  return err;
}

void
fla_base_pool_close(struct flexalloc *fs, struct fla_pool * handle)
{
  free(handle);
}


int
fla_pool_next_available_slab(struct flexalloc * fs, struct fla_pool_entry * pool_entry,
                             struct fla_slab_header ** slab)
{
  int err, ret;

  if(pool_entry->partial_slabs == FLA_LINKED_LIST_NULL)
  {
    if(pool_entry->empty_slabs == FLA_LINKED_LIST_NULL)
    {
      // ACQUIRE A NEW ONE
      err = fla_acquire_slab(fs, pool_entry->obj_nlb, slab);
      if(FLA_ERR(err, "fla_acquire_slab()"))
      {
        goto exit;
      }

      // Add to empty
      err = fla_hdll_prepend(fs, *slab, &pool_entry->empty_slabs);
      if(FLA_ERR(err, "fla_hdll_prepend()"))
      {
        goto release_slab;
      }

      goto exit;
release_slab:
      ret = fla_release_slab(fs, *slab);
      if(FLA_ERR(ret, "fla_release_slab()"))
      {
        goto exit;
      }
    }
    else
    {
      // TAKE FROM EMPTY
      *slab = fla_slab_header_ptr(pool_entry->empty_slabs, fs);
      if((err = -FLA_ERR(!slab, "fla_slab_header_ptr()")))
      {
        goto exit;
      }
    }
  }
  else
  {
    // TAKE FROM PARTIAL
    *slab = fla_slab_header_ptr(pool_entry->partial_slabs, fs);
    if((err = -FLA_ERR(!slab, "fla_slab_header_ptr()")))
    {
      goto exit;
    }
  }

exit:
  return err;
}

int
fla_slab_next_available_obj(struct flexalloc * fs, struct fla_slab_header * slab,
                            struct fla_object * obj, uint32_t num_objs)
{
  int err = 0;
  uint32_t slab_id;
  struct fla_pool_entry const * pool_entry;

  err = fla_slab_id(slab, fs, &slab_id);
  if(FLA_ERR(err, "fla_slab_id()"))
    return err;

  err = fla_slab_cache_obj_alloc(&fs->slab_cache, slab_id, obj, num_objs);
  if(FLA_ERR(err, "fla_slab_cache_obj_alloc()"))
    return err;

  pool_entry = &fs->pools.entries[slab->pool];
  slab->refcount += pool_entry->strp_nobjs;

  return err;
}

uint32_t*
fla_pool_best_slab_list(const struct fla_slab_header* slab,
                        struct fla_pool_entry * pool_entry)
{
  return slab->refcount == 0 ? &pool_entry->empty_slabs
         : slab->refcount + pool_entry->strp_nobjs > pool_entry->slab_nobj ? &pool_entry->full_slabs
         : &pool_entry->partial_slabs;
}

int
fla_base_object_open(struct flexalloc * fs, struct fla_pool * pool_handle,
                     struct fla_object * obj)
{
  int err;
  struct fla_slab_header * slab;
  struct fla_pool_entry const * pool_entry;

  slab = fla_slab_header_ptr(obj->slab_id, fs);
  if((err = FLA_ERR(!slab, "fla_slab_header_ptr()")))
    goto exit;

  pool_entry = &fs->pools.entries[pool_handle->ndx];
  err = fla_slab_cache_elem_load(&fs->slab_cache, obj->slab_id, pool_entry->slab_nobj);
  if(err == FLA_SLAB_CACHE_INVALID_STATE)
    err = 0; //Ignore as it was already loaded.
  else if(FLA_ERR(err, "fla_slab_cache_elem_load()"))
    goto exit;

  //FIXME : make sure that the object is taken in the free list.

exit:
  return err;
}

int
fla_base_object_create(struct flexalloc * fs, struct fla_pool * pool_handle,
                       struct fla_object * obj)
{
  int err;
  struct fla_slab_header * slab;
  struct fla_pool_entry * pool_entry;
  uint32_t * from_head, * to_head, slab_id;

  pool_entry = &fs->pools.entries[pool_handle->ndx];
  err = fla_pool_next_available_slab(fs, pool_entry, &slab);
  if(FLA_ERR(err, "fla_pool_next_available_slab()"))
  {
    goto exit;
  }

  err = fla_slab_id(slab, fs, &slab_id);
  if(FLA_ERR(err, "fla_slab_id()"))
  {
    goto exit;
  }

  err = fla_slab_cache_elem_load(&fs->slab_cache, slab_id, pool_entry->slab_nobj);
  if(err == FLA_SLAB_CACHE_INVALID_STATE)
    err = 0; //Ignore as it was already loaded.
  else if(FLA_ERR(err, "fla_slab_cache_elem_load()"))
  {
    FLA_DBG_EXEC(fla_print_fs(fs));
    goto exit;
  }

  from_head = fla_pool_best_slab_list(slab, pool_entry);

  err = fla_slab_next_available_obj(fs, slab, obj, pool_entry->strp_nobjs);
  if(FLA_ERR(err, "fla_slab_next_available_obj()"))
  {
    goto exit;
  }

  to_head = fla_pool_best_slab_list(slab, pool_entry);

  if(from_head != to_head)
  {
    // TODO: Add an assert to make sure that to_head != than from_head
    err = fla_hdll_remove(fs, slab, from_head);
    if(FLA_ERR(err, "fla_hdll_remove()"))
    {
      goto exit;
    }

    err = fla_hdll_prepend(fs, slab, to_head);
    if(FLA_ERR(err, "fla_hdll_prepend()"))
    {
      goto exit;
    }
  }

exit:
  return err;
}

uint64_t
fla_object_slba(struct flexalloc const * fs, struct fla_object const * obj,
                const struct fla_pool * pool_handle)
{
  const struct fla_pool_entry * pool_entry = &fs->pools.entries[pool_handle->ndx];
  return fla_geo_slab_lb_off(fs, obj->slab_id) + (pool_entry->obj_nlb * obj->entry_ndx);
}

uint64_t
fla_object_soffset(struct flexalloc const * fs, struct fla_object const * obj,
                   const struct fla_pool * pool_handle)
{
  return fla_object_slba(fs, obj, pool_handle) * fs->dev.lb_nbytes;
}

int
fla_base_object_destroy(struct flexalloc *fs, struct fla_pool * pool_handle,
                        struct fla_object * obj)
{
  int err = 0;
  struct fla_slab_header * slab;
  struct fla_pool_entry * pool_entry;
  uint32_t * from_head, * to_head;

  if (fla_geo_zoned(&fs->geo))
  {
    err = fla_znd_manage_zones_object_reset(fs, pool_handle, obj);
    if (FLA_ERR(err, "fla_xne_dev_znd_reset()"))
      goto exit;
  }

  slab = fla_slab_header_ptr(obj->slab_id, fs);
  if((err = FLA_ERR(!slab, "fla_slab_header_ptr()")))
    goto exit;

  pool_entry = &fs->pools.entries[pool_handle->ndx];
  from_head = fla_pool_best_slab_list(slab, pool_entry);

  err = fla_slab_cache_obj_free(&fs->slab_cache, obj);
  if(FLA_ERR(err, "fla_slab_cache_obj_free()"))
    goto exit;

  slab->refcount -= pool_entry->strp_nobjs;
  to_head = fla_pool_best_slab_list(slab, pool_entry);

  err = fla_hdll_remove(fs, slab, from_head);
  if(FLA_ERR(err, "fla_hdll_remove()"))
    goto exit;

  err = fla_hdll_prepend(fs, slab, to_head);
  if(FLA_ERR(err, "fla_hdll_prepend()"))
    goto exit;

exit:
  return err;
}

int
fla_slab_range_check_id(const struct flexalloc * fs, const uint32_t s_id)
{
  return s_id > fs->geo.nslabs - 1;
}

struct fla_slab_header *
fla_slab_header_ptr(const uint32_t s_id, const struct flexalloc * fs)
{
  int err = 0;
  err = fla_slab_range_check_id(fs,s_id);
  if(FLA_ERR(err, "fla_slab_range_check_id()"))
  {
    return NULL;
  }

  return fs->slabs.headers + s_id;
}

int
fla_slab_id(const struct fla_slab_header * slab, const struct flexalloc * fs,
            uint32_t * slab_id)
{
  int err = 0;
  *slab_id = (uint32_t)(slab - fs->slabs.headers);

  err = fla_slab_range_check_id(fs, *slab_id);
  FLA_ERR(err, "fla_slab_range_check_id()");

  return err;
}

int
fla_acquire_slab(struct flexalloc *fs, const uint32_t obj_nlb,
                 struct fla_slab_header ** a_slab)
{
  int err = 0;

  if(*fs->slabs.fslab_num == 0)
  {
    // TODO : Define a proper error code for this.
    /* This error should not print anything as it is a valid state.  */
    err = FLA_ERR_ALL_SLABS_USED;
    goto exit;
  }

  err = fla_edll_remove_head(fs, fs->slabs.fslab_head, fs->slabs.fslab_tail, a_slab);
  if(FLA_ERR(err, "fla_edll_remove_head()"))
  {
    goto exit;
  }

  err = fla_format_slab(fs, *a_slab, obj_nlb);
  if(FLA_ERR(err, "fla_format_slab()"))
  {
    goto exit;
  }

  (*fs->slabs.fslab_num)--;

exit:
  return err;
}

int
fla_release_slab(struct flexalloc *fs, struct fla_slab_header * r_slab)
{
  int err = 0;
  uint32_t slab_id;

  if(FLA_ERR(r_slab->refcount, "fla_release_slab()"))
  {
    // TODO : Valid error when application tells us to release pool when there are still objects.
    // Define a proper error code.
    err = 1;
    goto exit;
  }

  err = fla_slab_id(r_slab, fs, &slab_id);
  if(FLA_ERR(err, "fla_slab_id()"))
  {
    goto exit;
  }

  fla_slab_cache_elem_drop(&fs->slab_cache, slab_id);

  err = fla_edll_add_tail(fs, fs->slabs.fslab_head, fs->slabs.fslab_tail, r_slab);
  if(FLA_ERR(err, "fla_edll_add_tail()"))
  {
    goto exit;
  }

  (*fs->slabs.fslab_num)++;

exit:
  return err;
}

int
fla_format_slab(struct flexalloc *fs, struct fla_slab_header * slab, uint32_t obj_nlb)
{
  int err = 0;
  uint32_t obj_num, slab_id;

  obj_num = fla_calc_objs_in_slab(fs, obj_nlb);
  if((err = FLA_ERR(obj_num < 1, "No objects can fit in the slab")))
    goto exit;

  slab->next = FLA_LINKED_LIST_NULL;
  slab->prev = FLA_LINKED_LIST_NULL;
  slab->refcount = 0;

  // FIXME: Do we need the pool ID here?
  slab->pool = 0;

  err = fla_slab_id(slab, fs, &slab_id);
  if(FLA_ERR(err, "fla_slab_id()"))
    goto exit;

  err = fla_slab_cache_elem_init(&fs->slab_cache, slab_id, obj_num);
  if(FLA_ERR(err, "fla_slab_cache_elem_init()"))
    goto exit;

exit:
  return err;
}

uint64_t
fla_object_elba(struct flexalloc const * fs, struct fla_object const * obj,
                const struct fla_pool * pool_handle)
{
  const struct fla_pool_entry * pool_entry = &fs->pools.entries[pool_handle->ndx];
  return fla_geo_slab_lb_off(fs, obj->slab_id)
         + (pool_entry->obj_nlb * (obj->entry_ndx + pool_entry->strp_nobjs));
}

uint64_t
fla_object_eoffset(struct flexalloc const * fs, struct fla_object const * obj,
                   struct fla_pool const * pool_handle)
{
  return fla_object_elba(fs, obj, pool_handle) * fs->dev.lb_nbytes;
}

int
fla_object_read(const struct flexalloc * fs, struct fla_pool const * pool_handle,
                struct fla_object const * obj, void * buf, size_t r_offset, size_t r_len)
{
  int err;
  uint64_t obj_eoffset, obj_soffset, r_soffset, r_eoffset, slab_eoffset;
  struct fla_pool_entry *pool_entry = &fs->pools.entries[pool_handle->ndx];

  obj_eoffset = fla_object_eoffset(fs, obj, pool_handle);
  obj_soffset = fla_object_soffset(fs, obj, pool_handle);
  r_soffset = obj_soffset + r_offset;
  r_eoffset = r_soffset + r_len;

  slab_eoffset = (fla_geo_slab_lb_off(fs, obj->slab_id) + fs->geo.slab_nlb) * fs->geo.lb_nbytes;
  if((err = FLA_ERR(slab_eoffset < obj_eoffset, "Read outside a slab")))
    goto exit;

  if((err = FLA_ERR(obj_eoffset < r_eoffset, "Read outside of an object")))
    goto exit;

  if (pool_entry->strp_nobjs == 1)
    err = fla_xne_sync_seq_r_nbytes(fs->dev.dev, r_soffset, r_len, buf);
  else
  {
    struct fla_strp_params sp;
    sp.strp_nobjs = pool_entry->strp_nobjs;
    sp.strp_chunk_nbytes = pool_entry->strp_nbytes;
    sp.faobj_nlbs = pool_entry->obj_nlb;
    sp.xfer_snbytes = r_offset;
    sp.xfer_nbytes = r_len;
    sp.strp_obj_tnbytes = (uint64_t)pool_entry->strp_nobjs * pool_entry->obj_nlb * fs->geo.lb_nbytes;
    sp.strp_obj_start_nbytes = obj_soffset;
    sp.dev_lba_nbytes = fs->geo.lb_nbytes;
    sp.write = false;
    err = fla_xne_async_strp_seq_x(fs->dev.dev, buf, &sp);
  }

  if(FLA_ERR(err, "fla_xne_sync_seq_r_nbytes()"))
    goto exit;

exit:
  return err;
}

int
fla_object_write(struct flexalloc * fs, struct fla_pool const * pool_handle,
                 struct fla_object const * obj, void const * buf, size_t w_offset, size_t w_len)
{
  int err = 0;
  uint64_t obj_eoffset, obj_soffset, w_soffset, w_eoffset, slab_eoffset;
  struct fla_pool_entry *pool_entry = &fs->pools.entries[pool_handle->ndx];

  obj_eoffset = fla_object_eoffset(fs, obj, pool_handle);
  obj_soffset = fla_object_soffset(fs, obj, pool_handle);
  w_soffset = obj_soffset + w_offset;
  w_eoffset = w_soffset + w_len;

  slab_eoffset = (fla_geo_slab_lb_off(fs, obj->slab_id) + fs->geo.slab_nlb) * fs->geo.lb_nbytes;
  if((err = FLA_ERR(slab_eoffset < obj_eoffset, "Write outside slab")))
    goto exit;

  if((err = FLA_ERR(obj_eoffset < w_eoffset, "Write outside of an object")))
    goto exit;

  if (pool_entry->strp_nobjs == 1)
    err = fla_xne_sync_seq_w_nbytes(fs->dev.dev, w_soffset, w_len, buf);
  else
  {
    struct fla_strp_params sp;
    sp.strp_nobjs = pool_entry->strp_nobjs;
    sp.strp_chunk_nbytes = pool_entry->strp_nbytes;
    sp.faobj_nlbs = pool_entry->obj_nlb;
    sp.xfer_snbytes = w_offset;
    sp.xfer_nbytes = w_len;
    sp.strp_obj_tnbytes = (uint64_t)pool_entry->strp_nobjs * pool_entry->obj_nlb * fs->geo.lb_nbytes;
    sp.strp_obj_start_nbytes = obj_soffset;
    sp.dev_lba_nbytes = fs->geo.lb_nbytes;
    sp.write = true;
    err = fla_xne_async_strp_seq_x(fs->dev.dev, buf, &sp);
  }

  if(FLA_ERR(err, "fla_xne_sync_seq_w_nbytes()"))
    goto exit;

exit:
  return err;
}

int
fla_object_unaligned_write(struct flexalloc * fs, struct fla_pool const * pool_handle,
                           struct fla_object const * obj, void const * w_buf, size_t obj_offset,
                           size_t len)
{
  int err = 0;
  void * bounce_buf, * buf;
  size_t bounce_buf_size,  aligned_sb, aligned_eb, orig_sb, orig_eb;

  orig_sb = (fla_object_slba(fs, obj, pool_handle) * fs->dev.lb_nbytes) + obj_offset;
  orig_eb = orig_sb + len;
  aligned_sb = (orig_sb / fs->dev.lb_nbytes) * fs->dev.lb_nbytes;
  aligned_eb = FLA_CEIL_DIV(orig_eb, fs->dev.lb_nbytes) * fs->dev.lb_nbytes;
  bounce_buf_size = aligned_eb - aligned_sb;

  bounce_buf = fla_xne_alloc_buf(fs->dev.dev, bounce_buf_size);
  if(FLA_ERR(!bounce_buf, "fla_buf_alloc()"))
  {
    err = -ENOENT;
    goto exit;
  }

  if(aligned_sb < orig_sb)
  {
    err = fla_xne_sync_seq_r_nbytes(fs->dev.dev, aligned_sb, fs->dev.lb_nbytes, bounce_buf);
    if(FLA_ERR(err, "fla_xne_sync_seq_r_nbytes()"))
      goto free_bounce_buf;
  }

  if(aligned_eb > orig_eb)
  {
    buf = bounce_buf + bounce_buf_size - fs->dev.lb_nbytes;
    err = fla_xne_sync_seq_r_nbytes(fs->dev.dev, aligned_eb - fs->dev.lb_nbytes,
                                    fs->dev.lb_nbytes,
                                    buf);
    if(FLA_ERR(err, "fla_xne_sync_seq_r_nbytes()"))
      goto free_bounce_buf;
  }

  buf = bounce_buf + (orig_sb - aligned_sb);
  memcpy(buf, w_buf, len);

  err = fla_xne_sync_seq_w_nbytes(fs->dev.dev, aligned_sb, bounce_buf_size, bounce_buf);
  if(FLA_ERR(err, "fla_xne_sync_seq_w_nbytes()"))
    goto free_bounce_buf;

free_bounce_buf:
  free(bounce_buf);

exit:
  return err;
}

struct fla_object*
fla_pool_lookup_root_object(struct flexalloc const * const fs,
                            struct fla_pool const *pool_handle)
{
  struct fla_pool_entry *pool_entry;

  pool_entry = &fs->pools.entries[pool_handle->ndx];
  return (struct fla_object *)&pool_entry->root_obj_hndl;
}

int
fla_base_pool_set_root_object(struct flexalloc const * const fs,
                              struct fla_pool const * pool_handle,
                              struct fla_object const *obj, fla_root_object_set_action act)
{
  struct fla_object *pool_root;
  int ret = 0;

  // Lookup pool entry and check if root is already set
  pool_root = fla_pool_lookup_root_object(fs, pool_handle);
  if ((*(uint64_t *)pool_root != FLA_ROOT_OBJ_NONE) && !(act & ROOT_OBJ_SET_FORCE))
  {
    ret = EINVAL;
    FLA_ERR(ret, "Pool has root object set and force is false");
    goto out;
  }

  // Set the pool root to be the object root provided if clear is not set
  if (!(act & ROOT_OBJ_SET_CLEAR))
    *pool_root = *obj;
  else
    *(uint64_t *)pool_root = FLA_ROOT_OBJ_NONE;

out:
  return ret;
}

int
fla_base_pool_get_root_object(struct flexalloc const * const fs,
                              struct fla_pool const * pool_handle,
                              struct fla_object *obj)
{
  struct fla_object *pool_root;
  int ret = 0;

  // Lookup pool entry and check if it is not set
  pool_root = fla_pool_lookup_root_object(fs, pool_handle);
  if (*(uint64_t *)pool_root == FLA_ROOT_OBJ_NONE)
  {
    ret = EINVAL;
    goto out;
  }

  // Set obj to the pool root
  *obj = *pool_root;

out:
  return ret;

}

int32_t
fla_fs_lb_nbytes(struct flexalloc const * const fs)
{
  return fs->dev.lb_nbytes;
}

uint32_t
fla_pool_obj_nlb(struct flexalloc const * const fs, struct fla_pool const *pool_handle)
{
  struct fla_pool_entry * pool_entry;

  pool_entry = &fs->pools.entries[pool_handle->ndx];
  return pool_entry->obj_nlb;
}

uint64_t
fla_fs_nzsect(struct flexalloc const *fs)
{
  return fs->geo.nzsect;
}

bool
fla_fs_zns(struct flexalloc const *fs)
{
  return fla_geo_zoned(&fs->geo);
}

struct fla_fns base_fns =
{
  .close = &fla_base_close,
  .sync = &fla_base_sync,
  .pool_open = &fla_base_pool_open,
  .pool_close = &fla_base_pool_close,
  .pool_create = &fla_base_pool_create,
  .pool_destroy = &fla_base_pool_destroy,
  .pool_set_strp = &fla_base_pool_set_strp,
  .object_open = &fla_base_object_open,
  .object_create = &fla_base_object_create,
  .object_destroy = &fla_base_object_destroy,
  .pool_set_root_object = &fla_base_pool_set_root_object,
  .pool_get_root_object = &fla_base_pool_get_root_object,
};

int
fla_object_close(struct flexalloc *fs, struct fla_pool const *pool_handle, struct fla_object *obj)
{
  int err = 0;

  if (fla_fs_zns(fs))
  {
    err = fla_znd_manage_zones_object_finish(fs, pool_handle, obj);
    if (FLA_ERR(err, "fla_znd_manage_zones_object_finish()"))
      return err;
  }

  return err;
}

int
fla_open(struct fla_open_opts *opts, struct flexalloc **fs)
{
  struct xnvme_dev *dev = NULL, *md_dev = NULL;
  void *fla_md_buf;
  size_t fla_md_buf_len;
  struct fla_geo geo;
  struct fla_super *super;
  int err = 0;;

  (*fs) = fla_fs_alloc();
  if (!(*fs))
  {
    err = -ENOMEM;
    goto exit;
  }

  err = fla_xne_dev_open(opts->dev_uri, opts->opts, &dev);
  if (FLA_ERR(err, "fla_xne_dev_open()"))
    goto free_fs;

  if (opts->md_dev_uri)
  {
    err = fla_xne_dev_open(opts->md_dev_uri, NULL, &md_dev);
    if (FLA_ERR(err, "fla_xne_dev_open()"))
      goto xnvme_dev_close;
  }
  else
    md_dev = dev;

  err = fla_dev_sanity_check(dev, md_dev);
  if(FLA_ERR(err, "fla_dev_sanity_check()"))
    goto xnvme_dev_close;

  err = fla_super_read(md_dev, fla_xne_dev_lba_nbytes(dev), &super);
  if (FLA_ERR(err, "fla_super_read"))
    goto xnvme_dev_close;

  // read disk geometry
  fla_geo_from_super(dev, super, &geo);

  // allocate 1 big buffer to hold all metadata not internal to a slab.
  fla_md_buf_len = fla_geo_nbytes(&geo);
  fla_md_buf = fla_xne_alloc_buf(md_dev, fla_md_buf_len);
  if (FLA_ERR(!fla_md_buf, "fla_xne_alloc_buf()"))
  {
    err = -ENOMEM;
    goto free_super;
  }

  memset(fla_md_buf, 0, fla_md_buf_len);
  err = fla_xne_sync_seq_r_nbytes(md_dev, 0, fla_md_buf_len, fla_md_buf);
  if (FLA_ERR(err, "fla_xne_sync_seq_r_nbyte_nbytess()"))
    goto free_md;

  if (md_dev == dev)
    err = fla_init(&geo, dev, NULL, fla_md_buf, (*fs));
  else
    err = fla_init(&geo, dev, md_dev, fla_md_buf, (*fs));

  if (FLA_ERR(err, "fla_init()"))
    goto free_md;

  err = fla_slab_cache_init((*fs), &((*fs)->slab_cache));
  if (FLA_ERR(err, "fla_slab_cache_init()"))
    goto free_md;

  free(super);

  (*fs)->dev.dev_uri = strdup(opts->dev_uri);
  if ((*fs)->dev.dev_uri == NULL)
  {
    err = -ENOMEM;
    goto free_md;
  }

  if(md_dev != dev)
  {
    (*fs)->dev.md_dev_uri = strdup(opts->md_dev_uri);
    if ((*fs)->dev.md_dev_uri == NULL)
    {
      err = -ENOMEM;
      goto free_dev_uri;
    }
  }

  (*fs)->state |= FLA_STATE_OPEN;
  (*fs)->fns = base_fns;

  return 0;

free_dev_uri:
  free((*fs)->dev.dev_uri);
free_md:
  fla_xne_free_buf(md_dev, fla_md_buf);
free_super:
  fla_xne_free_buf(md_dev, super);
xnvme_dev_close:
  if (dev != md_dev)
    xnvme_dev_close(dev);

  xnvme_dev_close(md_dev);
free_fs:
  free(*fs);
exit:

  return err;
}


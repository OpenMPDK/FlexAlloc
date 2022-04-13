/**
 * flexalloc disk structures.
 *
 * Copyright (C) 2021 Jesper Devantier <j.devantier@samsung.com>
 * Copyright (C) 2021 Joel Granados <j.granados@samsung.com>
 * Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>
 *
 * @file flexalloc_mm.h
 */
#ifndef __FLEXALLOC_MM_H_
#define __FLEXALLOC_MM_H_
#include <stdint.h>
#include "flexalloc.h"

#define FLA_MAGIC 0x00534621 // 'flexalloc'
#define FLA_FMT_VER 1
#define FLA_SUPER_SLBA 0UL

#define FLA_STATE_OPEN 1U

#define FLA_NAME_SIZE 128
#define FLA_NAME_SIZE_POOL 112

#define FLA_ROOT_OBJ_NONE UINT64_MAX

#define FLA_MDTS_MIN_NBYTES 512

/// mkfs file system initialization parameters
struct fla_mkfs_p
{
  /// device URI, e.g. "/dev/null"
  char * dev_uri;
  /// md device URI, e.g. "/dev/ng0n2"
  char * md_dev_uri;
  /// size of each slab, in LBA's
  uint32_t slab_nlb;
  /// number of pools to support
  uint32_t npools;
  /// whether to be verbose during initialization
  uint8_t verbose;
};

/// an item representing a particular object in the freelist
struct fla_obj_list_item
{
  // both entries should be NULL if not in freelist.
  struct fla_obj_freelist_entry *prev, *next;
};

/// per-object metadata
/// (presently only the file name)
struct fla_obj_meta
{
  /// human-readable name for object, use object ID otherwise
  char name[FLA_NAME_SIZE];
};

/// Describes the layout and state of the slab itself.
struct fla_slab_header
{
  /// backpointer to parent pool
  uint64_t pool;
  uint32_t prev;
  uint32_t next;

  /// number of objects allocated from slab
  uint32_t refcount; // TODO: should have a var in cache structure describing n_entries/slab
};

struct fla_pool_htbl_header
{
  /// number of slots in the hash table
  uint32_t size;
  /// number of elements presently inserted
  uint32_t len;
};

struct fla_pool_entry
{
  /// identifier for each slab depending on its "fullness"
  uint32_t empty_slabs;
  uint32_t full_slabs;
  uint32_t partial_slabs;

  /// Number of LBA's used for each object in the cache
  ///
  /// Note that all objects in a cache have the same size.
  /// Also note that this is rounded up to fit alignment requirements.
  uint32_t obj_nlb;

  /// Number of Objects that fit in each slab
  uint32_t slab_nobj;

  /// Root object that is optionally set
  ///
  /// Pools can have any valid flexalloc object set as a root object
  uint64_t root_obj_hndl;

  /// Num of objects to stripe across
  ///
  /// Pools may optionally hand out striped objects
  uint32_t strp_nobjs;

  /// Number of bytes of each stripe chunk
  ///
  /// When striped, each fla object is subdivided into stripe
  /// subsection or chunks.
  /// Must be set if we set strp_nobjs
  uint32_t strp_nbytes;

  /// Human-readable cache identifier
  ///
  /// The cache name is primarily used for debugging statistics
  /// and when creating the read-only file system view.
  // TODO get rid of 512B block assumption
  char name[FLA_NAME_SIZE_POOL]; // maximize use of 512B block while having entries aligned by 8B

  //TODO : add a struct nested inside fla_pool_entry which holds just the metadata
  //       (everything but the name and name length). This will allow us to ignore the
  //       name when we need to.
};

/// flexalloc super block structure
///
/// The super block structure contains general information of the overall flexalloc
/// system and provides the LBA addresses for the cache and data segments.
struct fla_super
{
  /// magic identifier - used to determine if flexalloc
  uint64_t magic;

  /// Number of slab entries
  uint32_t nslabs;
  /// slab size, in LBA's
  uint32_t slab_nlb;

  /// Number of pool entries
  uint32_t npools;

  /// Blocks reserved for the super
  uint32_t md_nlb;

  /// flexalloc disk format version - permits backward compatibility
  uint8_t fmt_version;
};

/// calculate disk offset, in logical blocks, of the start of the slab identified by slab_id
/**
 * Calculate disk offset, in logical blocks, of the slab with id slab_id
 *
 * @param geo flexalloc system disk geometry
 * @param slab_id slab ID, a number from 0..N
 *
 * @return The logical block offset of the slab.
 */
uint64_t
fla_geo_slab_lb_off(struct flexalloc const *fs, uint32_t slab_id);

uint64_t
fla_geo_slab_sgmt_lb_off(struct fla_geo const *geo);

uint64_t
fla_geo_slabs_lb_off(struct fla_geo const *geo);

/**
 * Create new flexalloc system on disk
 * @param p parameters supplied (and inferred) from mkfs program
 *
 * @return On success 0.
 */
int
fla_mkfs(struct fla_mkfs_p *p);

/**
 * Flush flexalloc metadata to disk.
 *
 * Flush writes flexalloc metadata to disk, persisting any affecting pools and slabs
 * themselves.
 * NOTE: sync is NOT necessary to persist object writes.
 *
 * @return On success 0.
 */
int
fla_flush(struct flexalloc *fs);

/**
 * Close flexalloc system *without* writing changes to disk.
 *
 * Closes the flexalloc system without flushing changes to the metadata to the disk.
 * This is typically intended for read-only parsing of the flexalloc system state
 * and as an escape-hatch in case there is no time to wait for the write to succeed.
 *
 * NOTE: you will, by definition, lose changes if the meta data has changed since the
 * last flush to disk.
 *
 * @param fs flexalloc system handle
 */
void
fla_close_noflush(struct flexalloc *fs);


/**
 * @brief set initial pool_entry values for new/reset entry.
 *
 * Initializes the pool_entry as when newly acquired by a freshly created pool.
 * Chiefly, the name and obj_nlb entries are set while other accounting structures
 * are initialized to their default starting values.
 *
 * @param pool_entry the pool entry itself, should point to an entry in fs->pools.entries
 * @param name name given to the pool
 * @param name_len length of the pool name (should match strlen(name))
 * @param obj_nlb describes object size in the form of number of logical blocks
 * @param slab_nobj describes the number of objects of this pool that fit in a slab
 */
void
fla_pool_entry_reset(struct fla_pool_entry *pool_entry, const char *name, int name_len,
                     uint32_t const obj_nlb, uint32_t const slab_nobj);

/**
 * @brief Acquire the next free slab
 *
 * The next free slab gets assigned to slab_header and it is removed from the
 * free slab list. It is removed from the head of the free slab list.
 *
 * @param fs flexalloc system handle
 * @param obj_nlb size of object in logical blocks
 * @param slab_header Pointer that will get set to the next available slab
 * @return 0 on success. not zero otherwise.
 */
int
fla_acquire_slab(struct flexalloc *fs, const uint32_t obj_nlb,
                 struct fla_slab_header ** slab_header);

/**
 * @brief Add to the free slabs list
 *
 * It is appended to the tail of the free slab list.
 *
 * @param fs flexalloc system handle
 * @param slab_header pointer to slab that is to be released
 * @return zero on success. not zero otherwise.
 */
int
fla_release_slab(struct flexalloc *fs, struct fla_slab_header * slab_header);

/**
 * @brief Slab header pointer from slab ID
 *
 * Use the slab ID as an offset to find the slab header pointer.
 *
 * @param s_id Slab ID to search
 * @param fs flexalloc system handle
 * @return slab header pointer corresponding to the s_id
 */
struct fla_slab_header *
fla_slab_header_ptr(const uint32_t s_id, const struct flexalloc * fs);

/**
 * @brief Calculate slab id from slab header poiner
 *
 * @param slab Slab header pointer
 * @param fs flexalloc system handle
 * @param slab_id Where the resulting id is placed
 * @return zero on success. non zero otherwise.
 */
int
fla_slab_id(const struct fla_slab_header * slab, const struct flexalloc * fs,
            uint32_t * slab_id);

/**
 * @brief Initializes all in memory variables related to a slab header
 *
 * @param fs flexalloc system handle
 * @param slab slab header to initialize
 * @param obj_nlb size of the slab objects in logical blocks
 * @return zero on success. non zero otherwise.
 */
int
fla_format_slab(struct flexalloc *fs, struct fla_slab_header * slab, uint32_t obj_nlb);

uint64_t
fla_object_slba(struct flexalloc const * fs, struct fla_object const * obj,
                const struct fla_pool * pool_handle);

/**
 * @brief Opens a flexalloc device
 *
 * @param dev_uri path of the device to open
 * @param fs (uninitialized) flexalloc system handle
 * @return on success, 0 with *fs being initialized, non-zero otherwise.
 */
int
fla_open_common(char const *dev_uri, struct flexalloc *fs);

void
fla_print_fs(struct flexalloc *fs);
#endif // __FLEXALLOC_MM_H_


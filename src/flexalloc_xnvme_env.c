// Copyright (C) 2021 Joel Granados <j.granados@samsung.com>
// Copyright (C) 2021 Jesper Devantier <j.devantier@samsung.com>
// Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>

#include <libxnvme.h>
#include <libxnvme_dev.h>
#include <libxnvme_pp.h>
#include <libxnvme_lba.h>
#include <libxnvme_nvm.h>
#include <libxnvme_znd.h>
#include <libxnvmec.h>

#include <errno.h>
#include <stdint.h>
#include "flexalloc_xnvme_env.h"
#include "flexalloc_util.h"

uint32_t
fla_xne_calc_mdts_naddrs(const struct xnvme_dev * dev)
{
  const struct xnvme_geo *geo = xnvme_dev_get_geo(dev);
  return fla_min(geo->mdts_nbytes / geo->lba_nbytes, UINT16_MAX);
}

struct xnvme_lba_range
fla_xne_lba_range_from_slba_naddrs(struct xnvme_dev *dev, uint64_t slba, uint64_t naddrs)
{
  return xnvme_lba_range_from_slba_naddrs(dev, slba, naddrs);

}

struct xnvme_lba_range
fla_xne_lba_range_from_offset_nbytes(struct xnvme_dev *dev, uint64_t offset, uint64_t nbytes)
{
  return xnvme_lba_range_from_offset_nbytes(dev, offset, nbytes);
}

int
fla_xne_dev_mkfs_prepare(struct xnvme_dev *dev, char *md_dev_uri, struct xnvme_dev **md_dev)
{
  int err = 0;

  if (md_dev_uri)
  {
    if (fla_xne_dev_type(dev) == XNVME_GEO_ZONED)
    {
      err = fla_xne_dev_znd_send_mgmt(dev, 0, XNVME_SPEC_ZND_CMD_MGMT_SEND_RESET, true);
      if (FLA_ERR(err, "flexalloc_xne_dev_znd_reset()"))
        return err;
    }

    err = fla_xne_dev_open(md_dev_uri, NULL, md_dev);
    if (FLA_ERR(err,"failed to open md dev\n"))
      return -1;
  }

  return err;
}

int
fla_xne_dev_znd_send_mgmt(struct xnvme_dev *dev, uint64_t slba,
                          enum xnvme_spec_znd_cmd_mgmt_send_action act, bool all)
{
  int err = 0;
  uint32_t nsid;
  nsid = xnvme_dev_get_nsid(dev);
  struct xnvme_cmd_ctx ctx = xnvme_cmd_ctx_from_dev(dev);

  if (fla_xne_dev_type(dev) != XNVME_GEO_ZONED)
  {
    FLA_ERR_PRINT("Attempting zoned reset on a non zoned namespace\n");
    return -EINVAL;
  }

  err = xnvme_znd_mgmt_send(&ctx, nsid, slba, all, act, 0, NULL);
  if (err || xnvme_cmd_ctx_cpl_status(&ctx))
  {
    xnvmec_perr("xnvme_nvme_znd_mgmt_send", err);
    xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
    err = err ? err : -EIO;
  }

  return err;
}

uint32_t
fla_xne_dev_get_znd_mar(struct xnvme_dev *dev)
{
  const struct xnvme_spec_znd_idfy_ns *zns = (void *)xnvme_dev_get_ns_css(dev);
  return zns->mar;
}

uint32_t
fla_xne_dev_get_znd_mor(struct xnvme_dev *dev)
{
  const struct xnvme_spec_znd_idfy_ns *zns = (void *)xnvme_dev_get_ns_css(dev);
  return zns->mor;
}

int
fla_xne_sync_seq_w(const struct xnvme_lba_range * lba_range,
                   struct xnvme_dev * xne_hdl, const void * buf)
{
  int err = 0;
  uint32_t nsid;
  const char * wbuf = buf;
  struct xnvme_cmd_ctx ctx;

  nsid = xnvme_dev_get_nsid(xne_hdl);
  ctx = xnvme_cmd_ctx_from_dev(xne_hdl);

#ifdef FLA_XNVME_IGNORE_MDTS
  err = xnvme_nvm_write(&ctx, nsid, lba_range->slba, lba_range->naddrs - 1, wbuf, NULL);
  if (err || xnvme_cmd_ctx_cpl_status(&ctx))
  {
    xnvmec_perr("xnvme_nvm_write()", err);
    xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
    err = err ? err : -EIO;
    goto exit;
  }

#else
  uint32_t nlb, mdts_naddrs;
  const struct xnvme_geo * geo;
  mdts_naddrs = fla_xne_calc_mdts_naddrs(xne_hdl);
  geo = xnvme_dev_get_geo(xne_hdl);

  for(uint64_t slba = lba_range->slba; slba <= lba_range->elba; slba += mdts_naddrs)
  {
    /* mdts_naddrs -1 because it is not a zero based value */
    nlb = XNVME_MIN(lba_range->elba - slba, mdts_naddrs - 1);

    err = xnvme_nvm_write(&ctx, nsid, slba, nlb, wbuf, NULL);
    if (err || xnvme_cmd_ctx_cpl_status(&ctx))
    {
      xnvmec_perr("xnvme_nvm_write()", err);
      xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
      err = err ? err : -EIO;
      goto exit;
    }

    wbuf += (nlb+1)*geo->lba_nbytes;
  }
#endif //FLA_XNVME_IGNORE_MDTS

exit:
  return err;
}

int
fla_xne_sync_seq_w_naddrs(struct xnvme_dev * dev, const uint64_t slba, const uint64_t naddrs,
                          void const * buf)
{
  int err;
  struct xnvme_lba_range range;

  range = fla_xne_lba_range_from_slba_naddrs(dev, slba, naddrs);
  if ((err = FLA_ERR(range.attr.is_valid != 1, "fla_xne_lba_range_from_slba_naddrs()")))
    goto exit;

  err = fla_xne_sync_seq_w(&range, dev, buf);
  if (FLA_ERR(err, "fla_xne_sync_seq_w()"))
    goto exit;

exit:
  return err;
}

struct fla_async_cb_args
{
  uint32_t ecount;
  uint32_t completed;
  uint32_t submitted;
};

struct fla_async_strp_cb_args_common
{
  uint32_t nsid;
  void * buf;
  struct fla_strp_params *sp;
};

struct fla_async_strp_cb_args
{
  uint64_t slba;
  uint16_t nlbs;
  uint64_t sbuf_nbytes;
  struct fla_async_cb_args cb_args;
  struct fla_async_strp_cb_args_common * cmn_args;
};


/*
 * We can represent a zero start address by chunks in the following manner:
 * A = ((N*chunks) + left_over). N being the max number of chunks that can fit in A and
 * left_over is the number of addresses that are "left over" after (N*chunks).
 * This function changes N from being in chunks to being in translation_steps.
 */
static uint64_t
calc_chunk_translation(uint64_t const zero_offset, uint64_t const chunk,
                       uint64_t const translation_step)
{
  uint64_t offset_in_chunk = zero_offset % chunk;
  uint64_t offset_translation = ((zero_offset / chunk) * translation_step);
  return  offset_in_chunk + offset_translation;
}

static uint64_t
calc_strp_obj_slba(uint64_t const xfer_snbytes, struct fla_strp_params const * const sp)
{
  uint64_t chunks_in_faobs = calc_chunk_translation(xfer_snbytes, sp->strp_chunk_nbytes,
                             sp->faobj_nlbs * sp->dev_lba_nbytes);
  return calc_chunk_translation(chunks_in_faobs, sp->strp_obj_tnbytes, sp->strp_chunk_nbytes)
         / sp->dev_lba_nbytes;
}

static uint16_t
calc_strp_obj_next_nbls(uint64_t sbuf_nbytes,
                        struct fla_async_strp_cb_args_common const * const cmd_args)
{
  uint64_t nbytes_to_xfer = cmd_args->sp->xfer_nbytes - sbuf_nbytes;
  return (fla_min(cmd_args->sp->strp_chunk_nbytes,
                  nbytes_to_xfer) / cmd_args->sp->dev_lba_nbytes) - 1;
}

static uint16_t
calc_strp_obj_first_nlbs(uint64_t const xfer_snbytes,
                         struct fla_async_strp_cb_args_common const * const cmn_args)
{
  return (fla_min(cmn_args->sp->xfer_nbytes,
                  cmn_args->sp->strp_chunk_nbytes - (xfer_snbytes % cmn_args->sp->strp_chunk_nbytes))
          / cmn_args->sp->dev_lba_nbytes) - 1;
}

static uint64_t
calc_strp_obj_next_sbuf_nbytes(uint64_t const curr_sbuf_nbytes, uint32_t const xfered_nbls,
                               struct fla_async_strp_cb_args_common const * const cmn_args)
{
  return curr_sbuf_nbytes
         // adjust for the already tranfered bytes
         + ((xfered_nbls + 1) * cmn_args->sp->dev_lba_nbytes)

         // In order to "wrap around" to the next strp_nbytes on the device we need to
         // have transfered one strp_chunk_nbytes for each of the other non-striped
         // objects (not counting the current one)
         + (cmn_args->sp->strp_chunk_nbytes * (cmn_args->sp->strp_nobjs - 1));
}

/*
 * This callback is a little tricky and deserves a little comment. It is designed
 * to only tranfer data within a non-striped object. We can have a maximum of strp_nobjs of
 * these callbacks going on in parallel. Here we advance two pointers: 1. the starting logical
 * block addres (slba) and 2. the transfer buffer offset. Every time we advance the slba by
 * one strp_nbytes we need to move the buffer by strp_nbytes * (strp_nobjs-1) in order to keep
 * with the striping order. We stop when we have moved the buffer offset too far that it
 * goes over the original xfer_nbytes. We also need to make sure that we don't transfer too
 * many lbs and so we can our transfer to the xfer_nbytes.
 */
static void
fla_async_strp_cb(struct xnvme_cmd_ctx * ctx, void * cb_arg)
{
  int err;
  struct fla_async_strp_cb_args *cb_args = cb_arg;
  cb_args->cb_args.completed++;

  if (xnvme_cmd_ctx_cpl_status(ctx))
  {
    xnvme_cmd_ctx_pr(ctx, XNVME_PR_DEF);
    cb_args->cb_args.ecount++;
  }

  cb_args->sbuf_nbytes = calc_strp_obj_next_sbuf_nbytes(cb_args->sbuf_nbytes, cb_args->nlbs,
                         cb_args->cmn_args);

  if(cb_args->sbuf_nbytes < cb_args->cmn_args->sp->xfer_nbytes)
  {
    cb_args->slba += cb_args->nlbs + 1;
    cb_args->nlbs = calc_strp_obj_next_nbls(cb_args->sbuf_nbytes, cb_args->cmn_args);

    err = cb_args->cmn_args->sp->write
          ? xnvme_nvm_write(ctx, cb_args->cmn_args->nsid, cb_args->slba, cb_args->nlbs,
                            cb_args->cmn_args->buf + cb_args->sbuf_nbytes, NULL)
          : xnvme_nvm_read(ctx, cb_args->cmn_args->nsid, cb_args->slba, cb_args->nlbs,
                           cb_args->cmn_args->buf + cb_args->sbuf_nbytes, NULL);

    switch (err)
    {
    case 0:
      cb_args->cb_args.submitted +=1;
      break;

    case -EBUSY:
    case -EAGAIN:
    default:
      FLA_ERR(1, cb_args->cmn_args->sp->write ? "xnvme_nvm_write error" : "xnvme_nvm_read error");
      goto error;
    }
  }
  else
  {

error:
    err = xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
    FLA_ERR_ERRNO(err, "xnvme_queue_put_cmd_ctx");
  }
}

int
fla_xne_async_strp_seq_x(struct xnvme_dev *dev, void const *buf, struct fla_strp_params *sp)
{
  int err = 0, ret;
  struct xnvme_queue * queue = NULL;
  struct fla_async_strp_cb_args *cb_arg;
  struct xnvme_cmd_ctx *ctx;
  struct fla_async_strp_cb_args cb_args [512] = {0};
  struct fla_async_strp_cb_args_common cmn_args =
  {
    .nsid = xnvme_dev_get_nsid(dev),
    .buf = (char *)buf,
    .sp = sp,
  };

  if ((err = FLA_ERR(sp->xfer_nbytes % sp->dev_lba_nbytes || sp->xfer_snbytes % sp->dev_lba_nbytes,
                     "Transfer bytes (%"PRIu64") and start offset (%"PRIu64") " \
                     "must be aligned to block size (%"PRIu32")",
                     sp->xfer_nbytes, sp->xfer_snbytes, sp->dev_lba_nbytes)))
    goto exit;

  /*
   * 2 multiplier comes as a consequence when doing e.g. submission-upon-completion,
   * the queue is filled up consuming all entries. Then when processing a completion,
   * then the queue is not decremented until that completion-processing is done.
   * Thus you need more space in the queue, you then need a double amount size the
   * queue-size must be a power-of-2.
   */
  err = xnvme_queue_init(dev, sp->strp_nobjs * 2, 0, &queue);
  if (FLA_ERR(err, "xnvme_queue_init"))
    goto exit;

  uint64_t sbuf_nbytes = 0;
  for(uint32_t i = 0 ; i < sp->strp_nobjs && sbuf_nbytes < sp->xfer_nbytes; ++i)
  {
    ctx = xnvme_queue_get_cmd_ctx(queue);
    if((err = FLA_ERR_ERRNO(!ctx, "xnvme_queue_get_cmd_ctx")))
      goto close_queue;

    uint16_t curr_nlbs = calc_strp_obj_first_nlbs(sbuf_nbytes + sp->xfer_snbytes, &cmn_args);
    cb_arg = &cb_args[i];
    cb_arg->cmn_args = &cmn_args;
    cb_arg->nlbs = curr_nlbs;
    cb_arg->slba = calc_strp_obj_slba(sbuf_nbytes + sp->xfer_snbytes, sp)
                   + (sp->strp_obj_start_nbytes/sp->dev_lba_nbytes);
    cb_arg->sbuf_nbytes = sbuf_nbytes;

    xnvme_cmd_ctx_set_cb(ctx, fla_async_strp_cb, cb_arg);

submit:
    err = sp->write
          ? xnvme_nvm_write(ctx, cb_arg->cmn_args->nsid, cb_arg->slba, cb_arg->nlbs,
                            cb_arg->cmn_args->buf + cb_arg->sbuf_nbytes, NULL)
          : xnvme_nvm_read(ctx, cb_arg->cmn_args->nsid, cb_arg->slba, cb_arg->nlbs,
                           cb_arg->cmn_args->buf + cb_arg->sbuf_nbytes, NULL);

    switch (err)
    {
    case 0:
      cb_arg->cb_args.submitted +=1;
      break;

    case -EBUSY:
    case -EAGAIN:
      ret = xnvme_queue_poke(queue, 0);
      if((err = FLA_ERR_ERRNO(ret < 0, "xnvme_queue_poke")))
        goto close_queue;

      goto submit;

    default:
      FLA_ERR(1, "Async submission error\n");
      goto close_queue;
    }

    sbuf_nbytes += (curr_nlbs + 1) * sp->dev_lba_nbytes;
  }

close_queue:
  ret = xnvme_queue_wait(queue);
  if((err = FLA_ERR(ret < 0, "xnvme_queue_wait")))
    goto exit;

  if (queue)
  {
    err = xnvme_queue_term(queue);
    if(FLA_ERR(err, "xnvme_queue_term"))
      goto exit;
  }

  for(uint32_t i = 0 ; i < sp->strp_nobjs; ++i)
  {
    cb_arg = &cb_args[i];
    if ((err = FLA_ERR(cb_args->cb_args.ecount, "fla_xne_async_strp_seq_x")))
      goto exit;
  }

exit:
  return err;
}

int
fla_xne_sync_seq_w_nbytes(struct xnvme_dev * dev, const uint64_t offset, uint64_t nbytes,
                          void const * buf)
{
  int err;
  struct xnvme_lba_range range;

  range = fla_xne_lba_range_from_offset_nbytes(dev, offset, nbytes);
  if((err = FLA_ERR(range.attr.is_valid != 1, "fla_xne_lba_range_from_offset_nbytes()")))
    goto exit;

  err = fla_xne_sync_seq_w(&range, dev, buf);
  if (FLA_ERR(err, "fla_xne_sync_seq_w()"))
    goto exit;

exit:
  return err;
}


int
fla_xne_sync_seq_r(const struct xnvme_lba_range * lba_range,
                   struct xnvme_dev * xne_hdl, void * buf)
{
  int err = 0;
  uint32_t nsid;
  char * rbuf = buf;
  struct xnvme_cmd_ctx ctx;

  nsid = xnvme_dev_get_nsid(xne_hdl);
  ctx = xnvme_cmd_ctx_from_dev(xne_hdl);

#ifdef FLA_XNVME_IGNORE_MDTS
  err = xnvme_nvm_read(&ctx, nsid, lba_range->slba, lba_range->naddrs - 1, rbuf, NULL);
  if (err || xnvme_cmd_ctx_cpl_status(&ctx))
  {
    xnvmec_perr("xnvme_nvm_read()", err);
    xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
    err = err ? err : -EIO;
    goto exit;
  }
#else
  uint32_t nlb, mdts_naddrs;
  const struct xnvme_geo * geo;
  mdts_naddrs = fla_xne_calc_mdts_naddrs(xne_hdl);
  geo = xnvme_dev_get_geo(xne_hdl);
  for(uint64_t slba = lba_range->slba; slba <= lba_range->elba; slba += mdts_naddrs)
  {
    /* mdts_naddrs -1 because it is not a zero based value */
    nlb = XNVME_MIN(lba_range->elba - slba, mdts_naddrs - 1);

    err = xnvme_nvm_read(&ctx, nsid, slba, nlb, rbuf, NULL);
    if (err || xnvme_cmd_ctx_cpl_status(&ctx))
    {
      xnvmec_perr("xnvme_nvm_read()", err);
      xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
      err = err ? err : -EIO;
      goto exit;
    }
    rbuf += (nlb+1)*geo->lba_nbytes;
  }
#endif //FLA_XNVME_IGNORE_MDTS

exit:
  return err;
}

int
fla_xne_sync_seq_r_naddrs(struct xnvme_dev * dev, const uint64_t slba, const uint64_t naddrs,
                          void * buf)
{
  int err;
  struct xnvme_lba_range range;

  range = fla_xne_lba_range_from_slba_naddrs(dev, slba, naddrs);
  if((err = FLA_ERR(range.attr.is_valid != 1, "fla_xne_lba_range_from_slba_naddrs()")))
    goto exit;

  err = fla_xne_sync_seq_r(&range, dev, buf);
  if(FLA_ERR(err, "fla_xne_sync_seq_w()"))
    goto exit;

exit:
  return err;
}

int
fla_xne_sync_seq_r_nbytes(struct xnvme_dev * dev, const uint64_t offset, uint64_t nbytes,
                          void * buf)
{
  int err;
  struct xnvme_lba_range range;

  range = fla_xne_lba_range_from_offset_nbytes(dev, offset, nbytes);
  if((err = FLA_ERR(range.attr.is_valid != 1, "fla_xne_lba_range_from_slba_naddrs()")))
    goto exit;

  err = fla_xne_sync_seq_r(&range, dev, buf);
  if(FLA_ERR(err, "fla_xne_sync_seq_w()"))
    goto exit;

exit:
  return err;
}

int
fla_xne_write_zeroes(const struct xnvme_lba_range *lba_range,
                     struct xnvme_dev *xne_hdl)
{
  struct xnvme_cmd_ctx ctx;
  uint32_t nsid;
  int err = 0;

  nsid = xnvme_dev_get_nsid(xne_hdl);
  ctx = xnvme_cmd_ctx_from_dev(xne_hdl);

#ifdef FLA_XNVME_IGNORE_MDTS
  err = xnvme_nvm_write_zeroes(&ctx, nsid, lba_range->slba, lba_range->naddrs - 1);
  if (err || xnvme_cmd_ctx_cpl_status(&ctx))
  {
    xnvmec_perr("xnvme_nvm_write_zeroes()", err);
    xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
    err = err ? err : -EIO;
    goto exit;
  }
#else
  uint32_t nlb, mdts_naddrs;
  mdts_naddrs = fla_xne_calc_mdts_naddrs(xne_hdl);

  for (uint64_t slba = lba_range->slba; slba <= lba_range->elba; slba += mdts_naddrs)
  {
    /* mdtsnaddrs -1 because it is not a zero-based value */
    nlb = XNVME_MIN(lba_range->elba - slba, mdts_naddrs - 1);

    err = xnvme_nvm_write_zeroes(&ctx, nsid, slba, nlb);
    if (err || xnvme_cmd_ctx_cpl_status(&ctx))
    {
      xnvmec_perr("xnvme_nvm_write_zeroes()", err);
      xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
      err = err ? err : -EIO;
      goto exit;
    }
  }
#endif //FLA_XNVME_IGNORE_MDTS

exit:
  return err;
}

void *
fla_xne_alloc_buf(const struct xnvme_dev *dev, size_t nbytes)
{
  return xnvme_buf_alloc(dev, nbytes);
}

void *
fla_xne_realloc_buf(const struct xnvme_dev *dev, void *buf,
                    size_t nbytes)
{
  return xnvme_buf_realloc(dev, buf, nbytes);
}

void
fla_xne_free_buf(const struct xnvme_dev * dev, void * buf)
{
  xnvme_buf_free(dev, buf);
}

uint64_t
fla_xne_dev_tbytes(const struct xnvme_dev * dev)
{
  const struct xnvme_geo *geo = xnvme_dev_get_geo(dev);
  return geo->tbytes;
}

uint32_t
fla_xne_dev_lba_nbytes(const struct xnvme_dev * dev)
{
  struct xnvme_geo const * geo = xnvme_dev_get_geo(dev);
  return geo->lba_nbytes;
}

uint32_t
fla_xne_dev_znd_zones(const struct xnvme_dev *dev)
{
  struct xnvme_geo const *geo = xnvme_dev_get_geo(dev);
  return geo->nzone;
}

uint64_t
fla_xne_dev_znd_sect(const struct xnvme_dev *dev)
{
  struct xnvme_geo const *geo = xnvme_dev_get_geo(dev);
  return geo->nsect;
}

enum xnvme_geo_type
fla_xne_dev_type(const struct xnvme_dev *dev)
{
  struct xnvme_geo const *geo = xnvme_dev_get_geo(dev);
  return geo->type;
}

uint32_t
fla_xne_dev_mdts_nbytes(const struct xnvme_dev *dev)
{
  struct xnvme_geo const *geo = xnvme_dev_get_geo(dev);
  return geo->mdts_nbytes;
}

int
fla_xne_dev_open(const char *dev_uri, struct xnvme_opts *opts, struct xnvme_dev **dev)
{
  struct xnvme_opts default_opts;
  int err = 0;

  if (opts == NULL)
  {
    default_opts = xnvme_opts_default();
    opts = &default_opts;
    opts->direct = 1;
  }

  *dev = xnvme_dev_open(dev_uri, opts);
  if (FLA_ERR(!(*dev), "xnvme_dev_open()"))
  {
    err = 1001;
    return err;
  }

  return err;
}

void
fla_xne_dev_close(struct xnvme_dev *dev)
{
  xnvme_dev_close(dev);
}

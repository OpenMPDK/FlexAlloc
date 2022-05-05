"""
Copyright (C) 2021 Jesper Devantier <j.devantier@samsung.com>
"""
from libc.stdint cimport uint64_t, uint32_t, uint8_t


cdef extern from "libxnvme_lba.h":
    cdef struct xnvme_lba_range:
        uint64_t slba
        uint64_t elba
        uint32_t naddrs
        uint64_t nbytes
        #  actually a bitfield
        uint32_t attr

cdef extern from "libxnvme.h" nogil:
    """
    struct _xnvme_dev_opts {
        uint32_t value : 31;
        uint32_t given : 1;
    };
    """
    cdef struct _xnvme_opts_css:
        uint32_t value
        uint32_t given

    cdef struct xnvme_opts:
        const char *be
        const char *dev
        const char *mem
        const char *sync
        const char *async
        const char *admin
        uint32_t nsid
        ## flattened
        uint32_t rdonly
        uint32_t wronly
        uint32_t rdwr
        uint32_t create
        uint32_t truncate
        uint32_t direct
        uint32_t _rsvd
        uint32_t oflags
        ## /flattened
        uint32_t create_mode
        uint8_t poll_io
        uint8_t poll_sq
        uint8_t register_files
        uint8_t register_buffers
        _xnvme_opts_css css
        uint32_t use_cmb_sqs
        uint32_t shm_id
        uint32_t main_core
        const char *core_mask
        const char *adrfam
        uint32_t spdk_fabrics

cdef extern from "flexalloc_xnvme_env.h" nogil:
    # forward declaraction
    cdef struct xnvme_dev

    int fla_xne_sync_seq_w_naddrs(
            xnvme_dev *dev, const uint64_t slba, const uint64_t naddrs, const void *buf)
    int fla_xne_sync_seq_w_nbytes(
            xnvme_dev *dev, const uint64_t offset, uint64_t nbytes, const void *buf)
    int fla_xne_write_zeroes(
            const xnvme_lba_range *lba_range, xnvme_dev *dev)
    int fla_xne_sync_seq_r_naddrs(
            xnvme_dev *dev, uint64_t slba, uint64_t naddrs, void *buf)
    int fla_xnvme_sync_req_r_nbytes(
            xnvme_dev *dev, const uint64_t offset, uint64_t nbytes, void *buf)
    void * fla_xne_alloc_buf(const xnvme_dev *dev, size_t nbytes)
    void * fla_xne_realloc_buf(const xnvme_dev *dev, void *buf, size_t nbytes)
    void fla_xne_free_buf(xnvme_dev *dev, void *buf)
    uint64_t fla_xne_dev_tbytes(xnvme_dev *dev)
    uint32_t fla_xne_dev_lba_nbytes(xnvme_dev *dev)
    int fla_xne_dev_open(const char *dev_uri, xnvme_opts *opts, xnvme_dev **dev)
    void fla_xne_dev_close(xnvme_dev *dev)


cdef class XnvmeDev:
    cdef xnvme_dev *data
    cdef bint owner
    cdef bytes _dev_uri_py_str

    @staticmethod
    cdef public XnvmeDev from_ptr(xnvme_dev *ptr)

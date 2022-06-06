// Copyright (c) 2022-present, Adam Manzanares <a.manzanares@samsung.com>
// Copyright (c) 2022-present, Joel Granados <j.granados@samsung.com>
// Copyright (c) 2022-present, Jesper Devantier <j.devantier@samsung.com>

#pragma once
#include <iostream>
#include <mutex>
#include <flan.h>
#include <stdint.h>
#include "rocksdb/env.h"
#include "rocksdb/options.h"

#define ZNS_BLK_SZ 4096

namespace ROCKSDB_NAMESPACE {

std::unique_ptr<ROCKSDB_NAMESPACE::Env> NewFlanEnv(const std::string &flan_opts);

class LibFlanEnv : public EnvWrapper
{
  public:
    struct LibFlanEnv_F
    {
      LibFlanEnv_F():flanh(NULL), object_handle(0), e_o_f_(0), woffset_(0),
      roffset_(0), opens(0) {}
      struct flan_handle* flanh;
      uint64_t object_handle;
      uint64_t e_o_f_;
      off_t woffset_;
      off_t roffset_;
      uint32_t opens;
    };
    struct LibFlanEnv_Options
    {
      std::mutex *flan_mut;
    };

  private:
    std::string dev_uri = "";
    std::string md_dev_uri = "";
    struct flan_handle *flanh;
    std::shared_ptr<LibFlanEnv_Options> options;

  public:
    //explicit LibFlanEnv(const uint64_t target_file_size_base,
    //    const uint64_t max_manifest_file_size, const std::string &dev, const std::string &md_dev);
    explicit LibFlanEnv(Env *env, const std::string &flan_opts_file);

    virtual ~LibFlanEnv();

  public:
    Status NewSequentialFile(const std::string& fname, std::unique_ptr<SequentialFile>* result,
        const EnvOptions& options) override;
    Status NewRandomAccessFile(const std::string& fname, std::unique_ptr<RandomAccessFile>* result,
        const EnvOptions& options) override;
    Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result,
        const EnvOptions& options) override;

    Status NewDirectory(const std::string& name, std::unique_ptr<Directory>* result) override;
    Status FileExists(const std::string& fname) override;
    Status GetChildren(const std::string& path, std::vector<std::string>* result) override;
    Status DeleteFile(const std::string& fname) override;
    Status CreateDir(const std::string& name) override;
    Status CreateDirIfMissing(const std::string& name) override;
    Status DeleteDir(const std::string& name) override;
    Status GetFileSize(const std::string& fname, uint64_t* size) override;
    Status GetFileModificationTime(const std::string& fname, uint64_t* file_mtime) override;
    Status RenameFile(const std::string& src, const std::string& target) override;
    Status LinkFile(const std::string& src, const std::string& target) override;
    Status LockFile(const std::string& fname, FileLock** lock) override;
    Status UnlockFile(FileLock* lock) override;
    Status GetAbsolutePath(const std::string& db_path, std::string* output_path) override;
    Status GetTestDirectory(std::string* path) override;
    Status IsDirectory(const std::string& path, bool* is_dir) override;
    Status NewLogger(const std::string& fname, std::shared_ptr<Logger>* result) override;
    Status close();
    Status fssync();

};

class LibFlanEnvSeqAccessFile : public SequentialFile
{
private:
  std::string fname;
  struct flan_handle *flanh;
  std::shared_ptr<LibFlanEnv::LibFlanEnv_F> fh;
  std::mutex *lnfs_mut;

  Status ZNSRead(size_t n, Slice *result, char *scratch);

public:
  LibFlanEnvSeqAccessFile(
      const std::string & fname, std::shared_ptr<LibFlanEnv::LibFlanEnv_Options> options, struct flan_handle *flanh);
  virtual ~LibFlanEnvSeqAccessFile();

public:
  virtual Status Read(size_t n, Slice* result, char* scratch) override;
  virtual Status PositionedRead(uint64_t offset, size_t n, Slice* result, char* scratch) override;
  virtual Status Skip(uint64_t n) override;
  //virtual bool use_direct_io() const override;
  //virtual size_t GetRequiredBufferAlignment() const override;
};

class LibFlanEnvRandAccessFile : public RandomAccessFile
{
private:
  std::string fname;
  struct flan_handle *flanh;
  std::shared_ptr<LibFlanEnv::LibFlanEnv_F> fh;
  std::mutex *lnfs_mut;

public:
  LibFlanEnvRandAccessFile(
      const std::string & fname, std::shared_ptr<LibFlanEnv::LibFlanEnv_Options> options, struct flan_handle *flanh);
  virtual ~LibFlanEnvRandAccessFile();

public:
  virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override;
  virtual Status Prefetch(uint64_t offset, size_t n) override;
  virtual Status MultiRead(ReadRequest* reqs, size_t num_reqs) override;
  virtual size_t GetUniqueId(char* id, size_t max_size) const override;
  //virtual bool use_direct_io() const override;
  //virtual size_t GetRequiredBufferAlignment() const override;
  virtual Status InvalidateCache(size_t offset, size_t length) override;
};

class LibFlanEnvWriteableFile : public WritableFile
{
private:
  std::string fname;
  struct flan_handle *flanh;
  std::shared_ptr<LibFlanEnv::LibFlanEnv_F> fh;
  uint64_t _prevwr;
  std::mutex *lnfs_mut;

  Status ZNSAppend(const Slice& data);
  Status ZNSPositionedAppend(const Slice &data, uint64_t offset);

public:
  LibFlanEnvWriteableFile(
      const std::string & fname, std::shared_ptr<LibFlanEnv::LibFlanEnv_Options> options, struct flan_handle *flanh);
  virtual ~LibFlanEnvWriteableFile();

public:
  //virtual bool use_direct_io() const override;
  virtual Status Append(const Slice& data) override;
  virtual Status PositionedAppend(const Slice& data, uint64_t offset) override;
  virtual Status PositionedAppend(const Slice& s, uint64_t o, const DataVerificationInfo&) override;
  virtual Status Append(const Slice& data, const DataVerificationInfo&) override;
  virtual Status Flush() override;
  virtual Status Sync() override;
  virtual Status Close() override;
};

class LibFlanEnvDirectory : public Directory
{
public:
  LibFlanEnvDirectory();
  virtual ~LibFlanEnvDirectory();

public:
  virtual Status Fsync() override;

};

}

// Copyright (c) 2022-present, Adam Manzanares <a.manzanares@samsung.com>
// Copyright (c) 2022-present, Joel Granados <j.granados@samsung.com>
// Copyright (c) 2022-present, Jesper Devantier <j.devantier@samsung.com>

#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "env_flan.h"

#include <bits/stdint-uintn.h>
#include <memory>
#include <chrono>
#include <cstring>
#include <sstream>
#include <rocksdb/utilities/object_registry.h>


namespace ROCKSDB_NAMESPACE {
#define ROCKSDB_POOLNAME "ROCKSDB_POOL"

#ifndef ROCKSDB_LITE

extern "C" FactoryFunc<Env> flan_reg;

FactoryFunc<Env> flan_reg =
  ObjectLibrary::Default()->Register<Env>("flan:.*",
      [](const std::string &flan_opts, std::unique_ptr<Env>* env, std::string *) {
      *env = NewFlanEnv(flan_opts);
      return env->get();
      });

#endif // ROCKSDB_LITE

void split(const std::string &in, char delim, std::vector<std::string> &pieces)
{
  pieces.clear();
  size_t p_start = 0;
  size_t p_end = in.find(delim);
  while (p_end != std::string::npos) {
    std::string subpiece = in.substr(p_start, p_end - p_start);
    pieces.push_back(subpiece);
    p_start = p_end + 1;
    p_end = in.find(delim, p_start);
  }

  pieces.push_back(in.substr(p_start, in.size() - p_start));

}

std::unique_ptr<Env>
NewFlanEnv(const std::string &flan_opts) {
  return std::unique_ptr<Env>(new LibFlanEnv(Env::Default(), flan_opts));
}

LibFlanEnv::LibFlanEnv(Env *env, const std::string &flan_opts)
  : EnvWrapper(env), options(new LibFlanEnv_Options())
{
  uint64_t target_file_size_base;
  std::vector<std::string> opts;
  
  options->flan_mut = new std::mutex();
  // dev:target_file_size_base:md_dev (opt)
  split(flan_opts, ':', opts);


  if (opts.size() < 3 || opts.size() > 4 )
    throw std::runtime_error(std::string("flan params must be 2 or 3 colon separated values"));

  dev_uri = opts[1];
  std::istringstream conv(opts[2]);
  conv >> target_file_size_base;

  if (opts.size() == 4)
    md_dev_uri = opts[3];

  if (flan_init(dev_uri.c_str(), md_dev_uri.c_str(), ROCKSDB_POOLNAME, target_file_size_base / 64,
        &flanh))
      throw std::runtime_error(std::string("Error initializing flan:"));

  if (flan_pool_set_strp(flanh, 64, 2097152/64))
    throw std::runtime_error(std::string("Error setting flan striping parameters"));

}

LibFlanEnv::~LibFlanEnv()
{
  std::cout << "Executing the close function ===================================" << std::endl;
  //close();
}

Status
LibFlanEnv::NewSequentialFile(
    const std::string& fname, std::unique_ptr<SequentialFile>* result, const EnvOptions& options_)
{
  std::lock_guard<std::mutex> guard(*(options->flan_mut));
  result->reset(new LibFlanEnvSeqAccessFile(fname, options, flanh));
  return Status::OK();
}

Status
LibFlanEnv::NewRandomAccessFile(
    const std::string& fname, std::unique_ptr<RandomAccessFile>* result, const EnvOptions& options_)
{
  std::lock_guard<std::mutex> guard(*(options->flan_mut));
  result->reset(new LibFlanEnvRandAccessFile(fname, options, flanh));
  return Status::OK();
}

Status
LibFlanEnv::NewWritableFile(
    const std::string& fname, std::unique_ptr<WritableFile>* result, const EnvOptions& options_)
{
  std::lock_guard<std::mutex> guard(*(options->flan_mut));
  result->reset(new LibFlanEnvWriteableFile(fname, options, flanh));
  return Status::OK();
}

Status
LibFlanEnv::NewDirectory(const std::string& name, std::unique_ptr<Directory>* result)
{
  //std::lock_guard<std::mutex> guard(*(options->flan_mut));
  result->reset(new LibFlanEnvDirectory());
  return Status::OK();
}

Status
LibFlanEnv::FileExists(const std::string& fname)
{
  uint64_t oh;
  //std::lock_guard<std::mutex> guard(*(options->flan_mut));
  if(!flan_object_open(fname.c_str(), flanh, &oh, FLAN_OPEN_FLAG_READ)) {
    flan_object_close(oh, flanh);
    return Status::OK();
  }
  else
    return Status::NotFound();
}
Status
LibFlanEnv::GetChildren(const std::string& path, std::vector<std::string>* result)
{
  return Status::OK();
}

Status
LibFlanEnv::DeleteFile(const std::string& fname)
{
  //std::cout << "Calling " << __func__ << ", fname " << fname << std::endl;
  //std::lock_guard<std::mutex> guard(*(options->flan_mut));
  if(flan_object_delete(fname.c_str(), flanh))
    return Status::NotFound();

  return Status::OK(); // No matter what happens we return that the file is not there
}

Status
LibFlanEnv::CreateDir(const std::string& name)
{
  throw;
}

Status
LibFlanEnv::CreateDirIfMissing(const std::string& name)
{
  //std::cout << "Calling " << __func__ << ", fname " << name << std::endl;
  return Status::OK();
}

  Status
LibFlanEnv::DeleteDir(const std::string& name)
{
  //std::cout << "Calling " << __func__ << ", fname " << name << std::endl;
  return Status::OK();
}

  Status
LibFlanEnv::GetFileSize(const std::string& fname, uint64_t* size)
{
  //std::lock_guard<std::mutex> guard(*(options->flan_mut));
  struct flan_oinfo *oinfo = flan_find_oinfo(flanh, fname.c_str());
  
  if(!oinfo)
    return Status::NotFound();

  *size = oinfo->size;
  return Status::OK();
}

  Status
LibFlanEnv::GetFileModificationTime(const std::string& fname, uint64_t* file_mtime)
{
  throw;
}

Status
LibFlanEnv::RenameFile(const std::string& src, const std::string& target)
{
  //std::cout << "Calling " << __func__ << ", src " << src << ", target " << target << std::endl;
  //std::lock_guard<std::mutex> guard(*(options->flan_mut));
  
  if (flan_object_rename(src.c_str(), target.c_str(), flanh))
    return Status::NotFound();

  return Status::OK();
}

Status
LibFlanEnv::LinkFile(const std::string& src, const std::string& target)
{
  throw;
}
Status
LibFlanEnv::LockFile(const std::string& fname, FileLock** lock)
{
  //std::cout << "Calling " << __func__ << ", fname " << fname << std::endl;
  return Status::OK();
}
Status
LibFlanEnv::UnlockFile(FileLock* lock)
{
  //std::cout << "Calling " << __func__ << ", " << __FILE__ << __LINE__ << std::endl;
  return Status::OK();
}
Status
LibFlanEnv::GetAbsolutePath(const std::string& db_path, std::string* output_path)
{
  //std::cout << "Calling " << __func__ << ", fname " << db_path << std::endl;
  return Status::OK();
}
Status
LibFlanEnv::GetTestDirectory(std::string* path)
{
  //std::cout << __FUNCTION__ << std::endl;
  return Status::OK();
}
Status
LibFlanEnv::IsDirectory(const std::string& path, bool* is_dir)
{
  //std::cout << __FUNCTION__ << std::endl;
  throw;
}
Status
LibFlanEnv::NewLogger(const std::string& fname, std::shared_ptr<Logger>* result)
{
  //std::cout << "Calling " << __func__ << ", fname " << fname << std::endl;
  return Status::OK();
}

Status
LibFlanEnv::close()
{
  //std::lock_guard<std::mutex> guard(*(options->flan_mut));
  //std::cout << "BIG flush .... " << std::endl;
  flan_close(this->flanh); // TODO flan close returns status so should flan_close

  //if(flan_close(this->flan_handle) != 0)
  //{
  //  std::cerr << "Could not close flan handle." << std::endl;
  //  return Status::Aborted();
  //}
  //std::cout << "returning without an error" << std::endl;
  return Status::OK();
}

Status
LibFlanEnv::fssync()
{
  std::lock_guard<std::mutex> guard(*(options->flan_mut));
  //std::cout << "Flush has to wait until close " << std::endl;
  if(flan_sync(flanh) != 0)
  {
    std::cerr << "Aborting flushing ... " << std::endl;
	  return Status::Aborted();
  }
  return Status::OK();
}

LibFlanEnvSeqAccessFile::LibFlanEnvSeqAccessFile(
    const std::string & fname_, std::shared_ptr<LibFlanEnv::LibFlanEnv_Options> options, struct flan_handle *flanh_)
  :fname(fname_), flanh(flanh_), fh(NULL)
{
  //std::cout << "Seq Access File:" << fname << std::endl;
  lnfs_mut = options->flan_mut;
  fh = std::make_shared<LibFlanEnv::LibFlanEnv_F>();
  fh->flanh = flanh;
  if(flan_object_open(fname.c_str(), flanh, &fh->object_handle, FLAN_OPEN_FLAG_READ))
    throw std::runtime_error(std::string("Error finding file:") + fname);

  fh->opens++;
}

LibFlanEnvSeqAccessFile::~LibFlanEnvSeqAccessFile(){}

Status
LibFlanEnvSeqAccessFile::Read(size_t n, Slice* result, char* scratch)
{
  //std::cout << "Seq Access read:" << fname << " off:" << fh->roffset_ << " len:"  << n << std::endl;
  ssize_t len;
  std::lock_guard<std::mutex> guard(*lnfs_mut);

  len = flan_object_read(fh->object_handle, scratch, fh->roffset_, n, fh->flanh);
  if (len < 0)
    return Status::IOError();

  *result = Slice(scratch, len);
  fh->roffset_ += len;

  return Status::OK();
}

Status
LibFlanEnvSeqAccessFile::PositionedRead(uint64_t offset, size_t n, Slice* result, char* scratch)
{
  //std::cout << fh->offset_ << "," << fh->e_o_f_ << std::endl;
  //std::cout << "Seq Acc Pos read:" << fname << " off:" << offset << " len:"  << n << std::endl;
  ssize_t len;
  std::lock_guard<std::mutex> guard(*lnfs_mut);
  
  len = flan_object_read(fh->object_handle, scratch, offset, n, fh->flanh);
  if (len < 0)
    return Status::IOError();

  result->size_ = len;
  result->data_ = scratch;
  return Status::OK();
}

/*bool
LibFlanEnvSeqAccessFile::use_direct_io() const
{
  return true;
}*/

Status
LibFlanEnvSeqAccessFile::Skip(uint64_t n)
{
  if(fh->roffset_ + n > fh->e_o_f_)
    return Status::IOError();
  fh->roffset_ += n;
  return Status::OK();
}

LibFlanEnvRandAccessFile::LibFlanEnvRandAccessFile(
    const std::string & fname_, std::shared_ptr<LibFlanEnv::LibFlanEnv_Options> options, struct flan_handle *flanh_)
  :fname(fname_), flanh(flanh_), fh(NULL)
{
  //std::cout << "Rand Access File:" << fname << std::endl;
  lnfs_mut = options->flan_mut;
  fh = std::make_shared<LibFlanEnv::LibFlanEnv_F>();
  fh->flanh = flanh;
  if(flan_object_open(fname.c_str(), flanh, &fh->object_handle, FLAN_OPEN_FLAG_READ))
    throw std::runtime_error(std::string("Error allocating object for file:") + fname);
    
  fh->opens++;
}
LibFlanEnvRandAccessFile::~LibFlanEnvRandAccessFile(){}

Status
LibFlanEnvRandAccessFile::Read(uint64_t offset, size_t n, Slice* result, char* scratch) const
{
  //std::cout << "Calling a rand read offset : " << offset << " Size : "<< n << std::endl;
  //std::cout << "Ran Acc read:" << fname << " off:" << offset << " len"  << n << std::endl;
  ssize_t len;
  std::lock_guard<std::mutex> guard(*lnfs_mut);

  len = flan_object_read(fh->object_handle, scratch, offset, n, fh->flanh);
  if (len < 0)
    return Status::IOError();

  *result = Slice(scratch, len);
  return Status::OK();
}
Status
LibFlanEnvRandAccessFile::Prefetch(uint64_t offset, size_t n)
{
  return Status::OK();
}
Status
LibFlanEnvRandAccessFile::MultiRead(ReadRequest* reqs, size_t num_reqs)
{
  //std::cout << "Multi read " << std::endl;
  throw;
}
size_t
LibFlanEnvRandAccessFile::GetUniqueId(char* id, size_t max_size) const
{
  return 0;
};
/*bool
LibFlanEnvRandAccessFile::use_direct_io() const
{
  return true;
};*/
Status
LibFlanEnvRandAccessFile::InvalidateCache(size_t offset, size_t length)
{
  throw;
};

LibFlanEnvWriteableFile::LibFlanEnvWriteableFile(
    const std::string & fname_, std::shared_ptr<LibFlanEnv::LibFlanEnv_Options> options, struct flan_handle *flanh_)
  :fname(fname_), flanh(flanh_), fh(NULL), _prevwr(0)
{

  lnfs_mut = options->flan_mut;
  fh = std::make_shared<LibFlanEnv::LibFlanEnv_F>();
  fh->flanh = flanh;
  //auto start = std::chrono::high_resolution_clock::now();
  if(flan_object_open(fname.c_str(), flanh, &fh->object_handle, FLAN_OPEN_FLAG_CREATE | 
	  FLAN_OPEN_FLAG_WRITE))
    throw std::runtime_error(std::string("Error allocating object for file:") + fname);
    
  //auto end = std::chrono::high_resolution_clock::now();
  //auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  //std::cout << "Writeable File:" << fname << " : creation time(us):"<< duration.count() << std::endl;
  fh->opens++;
}

LibFlanEnvWriteableFile::~LibFlanEnvWriteableFile()
{
}

Status
LibFlanEnvWriteableFile::Append(const Slice& slice)
{
  std::lock_guard<std::mutex> guard(*lnfs_mut);
  //Env::num_written_bytes += slice.size_;

  //auto start = std::chrono::high_resolution_clock::now();
  if(flan_object_write(fh->object_handle, (void*)slice.data_, fh->woffset_, slice.size_, fh->flanh))
    throw std::runtime_error(std::string("Error appending to file"));

  //auto end = std::chrono::high_resolution_clock::now();
  //auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  //std::cout << "File:" << fname << " append size:" << slice.size_ << " wof:" << fh->woffset_ << std::endl;
  //std::cout << "Time(us)):" << duration.count() << std::endl;
  fh->e_o_f_ += slice.size_;
  fh->woffset_ += slice.size_;
  //std::cout << "buf : " << slice.data_ << " size : " << slice.data_ << std::endl;
  return Status::OK();
}

Status
LibFlanEnvWriteableFile::PositionedAppend(const Slice& slice, uint64_t offset)
{
  //std::cout << "File:" << fname << " pos append size:" << slice.size_<<  " pos:"<< offset<< std::endl;
  std::lock_guard<std::mutex> guard(*lnfs_mut);
  //Env::num_written_bytes += slice.size_;

  if(flan_object_write(fh->object_handle, (void*)slice.data_, offset, slice.size_, fh->flanh))
    throw std::runtime_error(std::string("Error pos appending to file"));

  fh->e_o_f_ = offset + slice.size_ > fh->e_o_f_ ? offset + slice.size_ : fh->e_o_f_;
  fh->woffset_ = offset + slice.size_;
  return Status::OK();
}

Status
LibFlanEnvWriteableFile::PositionedAppend(const Slice& s, uint64_t o, const DataVerificationInfo&)
{
  return PositionedAppend(s, o);
}

Status
LibFlanEnvWriteableFile::Append(const Slice& s, const DataVerificationInfo&)
{
  return Append(s);
}
/*
bool
LibFlanEnvWriteableFile::use_direct_io() const
{
  return true;
}*/

Status
LibFlanEnvWriteableFile::Flush()
{
  return Status::OK();
}
Status
LibFlanEnvWriteableFile::Sync()
{
  return Status::OK();
}
Status
LibFlanEnvWriteableFile::Close()
{
  //std::cout << "Calling " << __func__ << ", fname " << fname << std::endl;
  //std::lock_guard<std::mutex> guard(*lnfs_mut);
  flan_object_close(fh->object_handle, fh->flanh);
  return Status::OK();
}

LibFlanEnvDirectory::LibFlanEnvDirectory(){}
LibFlanEnvDirectory::~LibFlanEnvDirectory(){}
Status LibFlanEnvDirectory::Fsync(){return Status::OK();}

}

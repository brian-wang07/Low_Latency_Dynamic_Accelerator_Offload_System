#include <iostream>
#include <sys/stat.h> //mode constants
#include <fcntl.h>    //O_* constants
#include <unistd.h>
#include <cstring>
#include <sys/mman.h>

#ifdef __linux__
  #include <linux/mman.h>
#endif


#include "shm_manager.hpp"
#include "shm_types.hpp"

#ifdef __linux__
  // name_ is "/engine_shm_mvp"; hugetlbfs path is "/dev/hugepages/engine_shm_mvp"
  #define HUGEPAGES_PATH(name) ("/dev/hugepages" + (name))
  #define SHM_MMAP_FLAGS_HUGE (MAP_SHARED | MAP_HUGETLB | MAP_HUGE_2MB)
#endif
#define SHM_MMAP_FLAGS (MAP_SHARED)

bool ShmManager::create() {
  //creates a new shared memory object, and assigns addr_ to a pointer at the shared memory.

  if (is_valid_) {
    return false;
  }

  //initialize a new virtual memory block; creates the object with read/write access if it doesnt exist and fails if it does.
  //the object must be exclusive as well. owner has read and write permissions, group and other has read only.
#ifdef __linux__
  // try hugetlbfs first; falls back to posix shm if unavailable (e.g. WSL2)
  std::string hugepath = HUGEPAGES_PATH(name_);
  fd_ = ::open(hugepath.c_str(), O_CREAT | O_RDWR | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (fd_ != -1) {
    using_hugepages_ = true;
  } else {
    std::cerr << "warning: hugepages unavailable, falling back to POSIX shm (see README for setup)\n";
    fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  }
#else
  fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
#endif
  if (fd_ == -1) {
    //returns -1 on error
    std::perror("shm open failed");
    return false;
  }

  //set size of shm block to SHM_SIZE. ftruncate() returns -1 on error, and is declarative
  if (ftruncate(fd_, common::shm::SHM_SIZE) == -1) {
    ::close(fd_);
#ifdef __linux__
    if (using_hugepages_) ::unlink(hugepath.c_str()); else shm_unlink(name_.c_str());
#else
    shm_unlink(name_.c_str());
#endif
    fd_ = -1;
    return false;
  }

  //map p to the memory block. set addr as nullptr, to let kernel figure out location.
  //on Linux, MAP_HUGETLB | MAP_HUGE_2MB requests explicit 2MB hugepages from the hugetlbfs mount.
#ifdef __linux__
  int mmap_flags = using_hugepages_ ? SHM_MMAP_FLAGS_HUGE : SHM_MMAP_FLAGS;
#else
  int mmap_flags = SHM_MMAP_FLAGS;
#endif
  void *p = mmap(nullptr, common::shm::SHM_SIZE, PROT_READ | PROT_WRITE, mmap_flags, fd_, 0);
  if (p == MAP_FAILED) {
    ::close(fd_);
#ifdef __linux__
    if (using_hugepages_) ::unlink(hugepath.c_str()); else shm_unlink(name_.c_str());
#else
    shm_unlink(name_.c_str());
#endif
    fd_ = -1;
    return false;
  }

  addr_ = p;
  owner_ = true;
  is_valid_ = true;
  size_ = common::shm::SHM_SIZE;
  common::shm::shm_init_header(static_cast<common::shm::SharedMemoryBlock*>(addr_));
  return true;
}

bool ShmManager::open() {
  //opens an existing memory block. should return false if the block does not exist.

  if (is_valid_) {
    return false;
  }

#ifdef __linux__
  bool using_hugepages_ = false;
  std::string hugepath = HUGEPAGES_PATH(name_);
  fd_ = ::open(hugepath.c_str(), O_RDWR, 0);
  if (fd_ != -1) {
    using_hugepages_ = true;
  } else {
    fd_ = shm_open(name_.c_str(), O_RDWR, 0);
  }
#else
  fd_ = shm_open(name_.c_str(), O_RDWR, 0);
#endif
  if (fd_ == -1) {
    return false;
  }

#ifdef __linux__
  int mmap_flags = using_hugepages_ ? SHM_MMAP_FLAGS_HUGE : SHM_MMAP_FLAGS;
#else
  int mmap_flags = SHM_MMAP_FLAGS;
#endif
  void *p = mmap(nullptr, common::shm::SHM_SIZE, PROT_READ | PROT_WRITE, mmap_flags, fd_, 0);
  if (p == MAP_FAILED) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  auto* block = static_cast<common::shm::SharedMemoryBlock*>(p);
  if (!common::shm::shm_validate_header(block)) {
      std::cerr << "shm_open: layout mismatch (magic=" << block->header.magic
                << " version=" << block->header.version
                << "), expected magic=" << common::shm::SHM_MAGIC
                << " version=" << common::shm::SHM_VERSION << '\n';
      munmap(p, common::shm::SHM_SIZE);
      ::close(fd_);
      fd_ = -1;
      return false;
  }

  addr_ = p;
  owner_ = false;
  is_valid_ = true;
  size_ = common::shm::SHM_SIZE;
  return true;
}

void ShmManager::close() {
  if (addr_ != nullptr) {
    munmap(addr_, size_);
    addr_ = nullptr;
  }

  if (fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }

  is_valid_ = false;
  owner_ = false;
  size_ = 0;
}

ShmManager::~ShmManager() noexcept {
  if (owner_) {
    close();
#ifdef __linux__
    if (using_hugepages_) ::unlink(HUGEPAGES_PATH(name_).c_str()); else shm_unlink(name_.c_str());
#else
    shm_unlink(name_.c_str());
#endif
  } else {
    close();
  }
}

ShmManager::ShmManager(ShmManager&& other) noexcept
    : name_(std::move(other.name_)),
      fd_(other.fd_),
      addr_(other.addr_),
      size_(other.size_),
      owner_(other.owner_),
      is_valid_(other.is_valid_),
      using_hugepages_(other.using_hugepages_) {
  other.fd_ = -1;
  other.addr_ = nullptr;
  other.size_ = 0;
  other.owner_ = false;
  other.is_valid_ = false;
  other.using_hugepages_ = false;
}

ShmManager& ShmManager::operator=(ShmManager&& other) noexcept {
  if (this != &other) {
#ifdef __linux__
    if (owner_) { if (using_hugepages_) ::unlink(HUGEPAGES_PATH(name_).c_str()); else shm_unlink(name_.c_str()); }
#else
    if (owner_) shm_unlink(name_.c_str());
#endif
    close();
    name_ = std::move(other.name_);
    fd_ = other.fd_;
    addr_ = other.addr_;
    size_ = other.size_;
    owner_ = other.owner_;
    is_valid_ = other.is_valid_;
    using_hugepages_ = other.using_hugepages_;
    other.fd_ = -1;
    other.addr_ = nullptr;
    other.size_ = 0;
    other.owner_ = false;
    other.is_valid_ = false;
    other.using_hugepages_ = false;
  }
  return *this;
}

bool ShmManager::unlink() {
#ifdef __linux__
  if (using_hugepages_) return ::unlink(HUGEPAGES_PATH(name_).c_str()) == 0;
#endif
  return shm_unlink(name_.c_str()) == 0;
}

void* ShmManager::get_address() const noexcept { return addr_; }
std::size_t ShmManager::get_size() const noexcept { return size_; }
bool ShmManager::is_valid() const noexcept { return is_valid_; }
bool ShmManager::is_owner() const noexcept { return owner_; }

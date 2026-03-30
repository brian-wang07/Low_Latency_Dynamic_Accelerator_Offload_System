#include <iostream>
#include <sys/mman.h> //shm 
#include <sys/stat.h> //mode constants
#include <fcntl.h>    //O_* constants
#include <unistd.h>
#include <cstring>

#include "shm_manager.hpp"
#include "shm_types.hpp"

bool ShmManager::create() {
  //creates a new shared memory object, and assigns addr_ to a pointer at the shared memory.

  if (is_valid_) {
    return false;
  }

  //initialize a new virtual memory block; creates the object with read/write access if it doesnt exist and fails if it does.
  //the object must be exclusive as well. owner has read and write permissions, group and other has read only.  
  fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (fd_ == -1) {
    //returns -1 on error
    return false;
  }

  //set size of shm block to SHM_SIZE (1 mb). ftruncate() returns -1 on error, and is declarative
  if (ftruncate(fd_, common::shm::SHM_SIZE) == -1) {
    ::close(fd_);
    shm_unlink(name_.c_str());
    fd_ = -1;
    return false;
  }

  //map p to the memory block. set addr as nullptr, to let kernel figure out location. 
  void *p = mmap(nullptr, common::shm::SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (p == MAP_FAILED) {
    ::close(fd_);

    shm_unlink(name_.c_str());
    fd_ = -1;
    return false;
  }

  addr_ = p;
  owner_ = true;
  is_valid_ = true;
  size_ = common::shm::SHM_SIZE;
  std::memset(addr_, 0, size_);
  common::shm::shm_init_header(static_cast<common::shm::SharedMemoryBlock*>(addr_));
  return true;
}

bool ShmManager::open() {
  //opens an existing memory block. should return false if the block does not exist.

  if (is_valid_) {
    return false;
  }

  fd_ = shm_open(name_.c_str(), O_RDWR, 0);
  if (fd_ == -1) {
    return false;
  }

  void *p = mmap(nullptr, common::shm::SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
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
    shm_unlink(name_.c_str());
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
      is_valid_(other.is_valid_) {
  other.fd_ = -1;
  other.addr_ = nullptr;
  other.size_ = 0;
  other.owner_ = false;
  other.is_valid_ = false;
}

ShmManager& ShmManager::operator=(ShmManager&& other) noexcept {
  if (this != &other) {
    if (owner_) shm_unlink(name_.c_str());
    close();
    name_ = std::move(other.name_);
    fd_ = other.fd_;
    addr_ = other.addr_;
    size_ = other.size_;
    owner_ = other.owner_;
    is_valid_ = other.is_valid_;
    other.fd_ = -1;
    other.addr_ = nullptr;
    other.size_ = 0;
    other.owner_ = false;
    other.is_valid_ = false;
  }
  return *this;
}

bool ShmManager::unlink() {
  return shm_unlink(name_.c_str()) == 0;
}

void* ShmManager::get_address() const noexcept { return addr_; }
std::size_t ShmManager::get_size() const noexcept { return size_; }
bool ShmManager::is_valid() const noexcept { return is_valid_; }
bool ShmManager::is_owner() const noexcept { return owner_; }

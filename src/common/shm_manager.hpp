#pragma once

#include <sys/mman.h> //shm 
#include <sys/stat.h> //mode constants
#include <fcntl.h>    //O_* constants

class ShmManager {
public:
    ShmManager() = default;
    ~ShmManager();

    //no copy, move is okay
    ShmManager(const ShmManager&) = delete;
    ShmManager& operator=(const ShmManager&) = delete;

    ShmManager(ShmManager&& other) noexcept;
    ShmManager& operator=(ShmManager&& other) noexcept;

    bool is_owner() const noexcept;
    std::size_t size() const noexcept;

private:
    std::string name_;
    int fd_ = -1;
    void *addr_ = nullptr;
    std::size_t size_ = 0;
    bool owner_ = false;
};
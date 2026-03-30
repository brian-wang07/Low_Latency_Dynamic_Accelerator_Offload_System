#pragma once

#include <cstddef>
#include <string>

#include "shm_types.hpp"

#include <sys/mman.h> //shm 
#include <sys/stat.h> //mode constants
#include <fcntl.h>    //O_* constants

class ShmManager {
public:
    ShmManager() = default;
    ~ShmManager() noexcept;

    //no copy, move is okay
    ShmManager(const ShmManager&) = delete;
    ShmManager& operator=(const ShmManager&) = delete;

    ShmManager(ShmManager&& other) noexcept;
    ShmManager& operator=(ShmManager&& other) noexcept;
 
    bool create();
    bool open();
    void close();
    bool unlink();

    void *get_address() const noexcept;
    std::size_t get_size() const noexcept;
    bool is_valid() const noexcept;
    bool is_owner() const noexcept;

    template<typename T> 
    T* as() const noexcept {
        return std::launder(reinterpret_cast<T*>(addr_));
    }


private:
    std::string name_ = common::shm::SHM_NAME;
    int fd_ = -1;
    void *addr_ = nullptr;
    std::size_t size_ = 0;
    bool owner_ = false;
    bool is_valid_ = false;
};

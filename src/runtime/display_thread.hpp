#pragma once
#include <csignal>
#include "../common/shm_types.hpp"

class DisplayThread {
public:
    void run(const dashboard::shm::BookSnapshot& snapshot, volatile sig_atomic_t& running);
};

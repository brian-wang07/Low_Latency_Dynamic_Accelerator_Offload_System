#pragma once
#include <csignal>
#include "book_snapshot.hpp"

class DisplayThread {
public:
    void run(const BookSnapshot& snapshot, volatile sig_atomic_t& running);
};

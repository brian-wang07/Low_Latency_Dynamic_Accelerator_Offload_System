#include <iostream>
#include <thread>
#include <chrono>

#include "shm_types.hpp"

int main() {
    std::cout << "Starting Runtime Engine process..." << std::endl;
    // TODO: Attach to shared memory
    // TODO: Busy-poll input for new market data (zero-copy)
    // TODO: Pass data to accelerator
    // TODO: Poll for accelerator signal
    
    while(true) {
        // Poll input
        std::this_thread::yield();
    }
    
    return 0;
}

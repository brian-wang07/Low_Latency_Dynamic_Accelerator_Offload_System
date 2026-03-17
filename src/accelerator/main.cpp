#include <iostream>
#include <thread>
#include <chrono>

#include "shm_types.hpp"

int main() {
    std::cout << "Starting Accelerator Simulator process..." << std::endl;
    // TODO: Attach to shared memory
    // TODO: Busy-poll for data from Runtime
    // TODO: Compute simple signal
    // TODO: Write signal back to shared memory via atomics
    
    while(true) {
        // Poll input
        std::this_thread::yield();
    }

    return 0;
}

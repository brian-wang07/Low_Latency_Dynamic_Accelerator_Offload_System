Kernel-aware userspace system for dynamic accelerator offload under market microbursts

3 processes: Data, accelerator, runtime
Data: Random walk to simulate market data. Implement microbursts as well
Accelerator: Implement a simple signal on a custom accelerator. Use shm so that the accelerator and runtime can communicate quickly
Runtime: Instead of waiting for the cpu to tell if there is new data, constantly poll the input to check for new incoming data. Later: Use vfio to route this directly to the accelerator
Features: zero copy, memory semantics (std::mutex is too slow, use atomics). Expose accelerator interface so that new strategy implementation is easy, abstract everything else away
Benchmark against regular cpu/research question: for a single packet, it will be faster (pcie is in order of microseconds), at what queue depth/burst intensity does the overhead for offloading to accelerator win?
Tools: C++ (pref. 23 or newer), libvfio-user (simulate accelerator as something like gpu or network card), hugepages (custom memory mapping), vfio-ioctl (emulate pcie accelerator)
Create mvp using shared memory, later implement vfio-ioctl pcie emulation


cmake_minimum_required(VERSION 3.20)
project(LowLatencyEngine VERSION 0.1.0 LANGUAGES CXX)

# Use C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Add common includes
add_library(common INTERFACE)
target_include_directories(common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/common)

# Process 1: Data Simulator
add_executable(data_sim src/data/main.cpp)
target_link_libraries(data_sim PRIVATE common Threads::Threads rt)

# Process 2: Runtime Engine
add_executable(runtime_engine src/runtime/main.cpp)
target_link_libraries(runtime_engine PRIVATE common Threads::Threads rt)

# Process 3: Accelerator Simulator
add_executable(accelerator_sim src/accelerator/main.cpp)
target_link_libraries(accelerator_sim PRIVATE common Threads::Threads rt)

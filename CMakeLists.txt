cmake_minimum_required(VERSION 3.20)
project(LowLatencyEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# Use C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Add common includes
add_library(common STATIC src/common/shm_manager.cpp)
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/common)

# DataGenerator library (shared between data_sim and tests)
add_library(data_generator_lib STATIC src/data/data_generator.cpp)
target_include_directories(data_generator_lib PUBLIC src/data)
target_link_libraries(data_generator_lib PUBLIC common)

# Process 1: Data Simulator
add_executable(data_sim src/data/main.cpp)
target_compile_definitions(data_sim PRIVATE PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(data_sim PRIVATE data_generator_lib Threads::Threads rt)

# Tests
add_executable(data_test src/tests/data.cpp)
target_link_libraries(data_test PRIVATE data_generator_lib Threads::Threads rt)

# Order book test
add_executable(order_book_test src/tests/order_book_test.cpp src/runtime/order_book.cpp)
target_include_directories(order_book_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common)

# Process 2: Runtime Engine
add_executable(runtime_engine src/runtime/main.cpp src/runtime/runtime_engine.cpp src/runtime/order_book.cpp)
target_include_directories(runtime_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime)
target_link_libraries(runtime_engine PRIVATE common Threads::Threads rt)

# Process 3: Accelerator Simulator
add_executable(accelerator_sim src/accelerator/main.cpp)
target_link_libraries(accelerator_sim PRIVATE common Threads::Threads rt)

cmake_minimum_required(VERSION 3.20)

# On macOS, Apple Clang is required for Objective-C++ (GLFW/Cocoa headers).
# Homebrew GCC cannot parse macOS SDK Objective-C extensions.
if(APPLE AND NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(APPLE_CLANGXX NAMES clang++ PATHS /usr/bin NO_DEFAULT_PATH)
    if(APPLE_CLANGXX)
        set(CMAKE_CXX_COMPILER "${APPLE_CLANGXX}")
        set(CMAKE_OBJCXX_COMPILER "${APPLE_CLANGXX}")
    endif()
endif()

if(APPLE)
    project(LowLatencyEngine VERSION 0.1.0 LANGUAGES CXX OBJCXX)
else()
    project(LowLatencyEngine VERSION 0.1.0 LANGUAGES CXX)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v0.16
)
FetchContent_MakeAvailable(implot)

FetchContent_Declare(
    jetbrains_mono
    GIT_REPOSITORY https://github.com/JetBrains/JetBrainsMono.git
    GIT_TAG        v2.304
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(jetbrains_mono)

find_package(glfw3 3.3 REQUIRED)
find_package(OpenGL REQUIRED)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

add_library(implot_lib STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(implot_lib PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot_lib PUBLIC imgui_lib)

# On macOS, GLFW headers pull in Cocoa which uses Objective-C blocks (^).
# Compile the GLFW and OpenGL backends as Objective-C++ to handle this.
if(APPLE)
    set_source_files_properties(
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        PROPERTIES LANGUAGE OBJCXX
    )
endif()

# Use C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# librt is needed on Linux for shm_open; macOS has it in libc
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(RT_LIB rt)
else()
    set(RT_LIB "")
endif()

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
target_link_libraries(data_sim PRIVATE data_generator_lib Threads::Threads ${RT_LIB})

# Tests
add_executable(data_test src/tests/data.cpp)
target_link_libraries(data_test PRIVATE data_generator_lib Threads::Threads ${RT_LIB})

# Order book test
add_executable(order_book_test src/tests/order_book_test.cpp src/runtime/order_book.cpp)
target_include_directories(order_book_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common)

# Process 2: Runtime Engine
add_executable(runtime_engine
    src/runtime/main.cpp
    src/runtime/runtime_engine.cpp
    src/runtime/order_book.cpp
    src/runtime/display_thread.cpp
)
target_include_directories(runtime_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime)
target_link_libraries(runtime_engine PRIVATE common Threads::Threads ${RT_LIB} imgui_lib implot_lib)
target_compile_definitions(runtime_engine PRIVATE
    JETBRAINS_MONO_FONT="${jetbrains_mono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Regular.ttf"
)

# Process 3: Accelerator Simulator
add_executable(accelerator_sim src/accelerator/main.cpp)
target_link_libraries(accelerator_sim PRIVATE common Threads::Threads ${RT_LIB})

cmake_minimum_required(VERSION 3.16)

project(mem_manager
    VERSION 0.1.0
    DESCRIPTION "A memory manager"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Core library (the actual memory-manager logic lives here so it can be tested).
add_library(mem_manager_lib
    src/mem_manager.cpp)
target_include_directories(mem_manager_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
    target_compile_options(mem_manager_lib PRIVATE /W4)
else()
    target_compile_options(mem_manager_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Executable entry point.
add_executable(mem_manager src/main.cpp)
target_link_libraries(mem_manager PRIVATE mem_manager_lib)

# Tests (built with CTest; toggle with -DMEM_MANAGER_BUILD_TESTS=OFF).
option(MEM_MANAGER_BUILD_TESTS "Build unit tests" ON)
if(MEM_MANAGER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

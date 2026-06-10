cmake_minimum_required(VERSION 3.16)

project(mem_manager
    VERSION 0.1.0
    DESCRIPTION "Shared-memory pooling: Platform_Shm* wrappers (Step 1: profiling)"
    LANGUAGES C)

# Written in C so the DP-plane (C-only) toolchain can compile it; a C++ control
# plane calls the same functions via the extern "C" headers.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# When ON, INT32/uint32 and the OS Shm* functions come from the business api.h
# instead of mem-manager's standalone shims + heap stub.
option(MEM_MANAGER_USE_API_H "Pull types and OS Shm* from the real api.h" OFF)
option(MEM_MANAGER_BUILD_TESTS "Build unit tests" ON)

find_package(Threads REQUIRED)

set(MEM_MANAGER_SOURCES
    src/platform_shm.c
    src/profiler/shm_profiler.c)

if(NOT MEM_MANAGER_USE_API_H)
    list(APPEND MEM_MANAGER_SOURCES src/os/os_shm_stub.c)
endif()

add_library(mem_manager_lib ${MEM_MANAGER_SOURCES})
target_include_directories(mem_manager_lib
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mem_manager_lib PUBLIC Threads::Threads)

# Step-1 profiling is enabled in Debug builds only; Release is pure pass-through
# and shm_profiler.c compiles to an empty translation unit (no pthread use).
target_compile_definitions(mem_manager_lib PUBLIC $<$<CONFIG:Debug>:MEM_MANAGER_PROFILE>)
if(MEM_MANAGER_USE_API_H)
    target_compile_definitions(mem_manager_lib PUBLIC MEM_MANAGER_USE_API_H)
endif()

if(MSVC)
    target_compile_options(mem_manager_lib PRIVATE /W4)
else()
    target_compile_options(mem_manager_lib PRIVATE -Wall -Wextra)
endif()

add_executable(mem_manager src/main.c)
target_link_libraries(mem_manager PRIVATE mem_manager_lib)

if(MEM_MANAGER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

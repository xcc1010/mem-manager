# mem-manager

A memory manager written in C++.

## Requirements

- A C++20 compiler (MSVC, GCC, or Clang)
- CMake 3.16+

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Run

```sh
./build/mem_manager        # Linux/macOS
.\build\Debug\mem_manager.exe   # Windows (MSVC)
```

## Test

```sh
ctest --test-dir build --output-on-failure
```

## Project layout

```
include/mem_manager/   public headers
src/                   library + executable sources
tests/                 unit tests (CTest)
```

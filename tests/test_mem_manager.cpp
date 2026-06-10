#include <cassert>
#include <iostream>

#include "mem_manager/mem_manager.hpp"

int main() {
    assert(mem_manager::version() == "0.1.0");
    std::cout << "all tests passed\n";
    return 0;
}

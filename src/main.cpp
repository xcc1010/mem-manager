#include <iostream>

#include "mem_manager/mem_manager.hpp"

int main() {
    std::cout << "mem-manager " << mem_manager::version() << '\n';
    return 0;
}

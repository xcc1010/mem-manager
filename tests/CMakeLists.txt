# Minimal test target driven by CTest. No external framework: asserts + exit code.
add_executable(mem_manager_tests test_mem_manager.cpp)
target_link_libraries(mem_manager_tests PRIVATE mem_manager_lib)

add_test(NAME mem_manager_tests COMMAND mem_manager_tests)

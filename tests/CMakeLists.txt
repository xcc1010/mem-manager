# Minimal test target driven by CTest. No external framework: asserts + exit code.
add_executable(mem_manager_tests test_mem_manager.c)
target_link_libraries(mem_manager_tests PRIVATE mem_manager_lib)

add_test(NAME mem_manager_tests COMMAND mem_manager_tests)

# Pool test builds only when the pool is compiled into the library.
if(MEM_MANAGER_POOL)
    add_executable(mem_manager_pool_tests test_pool.c)
    # test includes pool/mm_pool.h, which lives under src/ (PRIVATE on the lib)
    target_include_directories(mem_manager_pool_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(mem_manager_pool_tests PRIVATE mem_manager_lib)
    add_test(NAME mem_manager_pool_tests COMMAND mem_manager_pool_tests)
endif()

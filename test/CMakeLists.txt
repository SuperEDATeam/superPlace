set(SP_TEST_DATA_DIR "${CMAKE_SOURCE_DIR}/test_data")

function(sp_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE superplace)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE SP_TEST_DATA_DIR="${SP_TEST_DATA_DIR}")
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

sp_add_test(test_parser)
sp_add_test(test_csr)
sp_add_test(test_roundtrip)

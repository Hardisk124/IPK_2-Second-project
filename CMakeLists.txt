cmake_minimum_required(VERSION 3.16)
project(ipk_rdt C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(ipk-rdt
  src/main.c
  src/client.c
  src/server.c
  src/protocol.c
)

target_compile_options(ipk-rdt PRIVATE -Wall -Wextra -pedantic -O2)

enable_testing()

add_executable(ipk-tests
  tests/test_runner.c
)

target_include_directories(ipk-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(ipk-tests PRIVATE -Wall -Wextra -pedantic -O2)

function(add_ipk_test name test_case)
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND} -E env IPK_RDT_BIN=$<TARGET_FILE:ipk-rdt> $<TARGET_FILE:ipk-tests> ${test_case}
  )
  set_tests_properties(${name} PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RUN_SERIAL TRUE
  )
endfunction()

add_ipk_test(basic_file_transfer basic_file_transfer)
add_ipk_test(stdin_stdout_transfer stdin_stdout_transfer)
add_ipk_test(stdin_to_file_transfer stdin_to_file_transfer)
add_ipk_test(file_to_stdout_transfer file_to_stdout_transfer)
add_ipk_test(empty_input_transfer empty_input_transfer)
add_ipk_test(cli_invalid_args cli_invalid_args)
add_ipk_test(client_timeout client_timeout)
add_ipk_test(prehandshake_garbage prehandshake_garbage)
add_ipk_test(ipv6_transfer ipv6_transfer)
add_ipk_test(netem_impairments netem_impairments)

set_tests_properties(ipv6_transfer PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(netem_impairments PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(prehandshake_garbage PROPERTIES SKIP_RETURN_CODE 77)

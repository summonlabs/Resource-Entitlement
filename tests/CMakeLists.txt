# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

add_library(entl_test_support STATIC
    harness.cpp
    test_support.cpp)
target_include_directories(entl_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(entl_test_support PUBLIC resource_entitlement)
resource_entitlement_strict_warnings(entl_test_support)

# Every test executable is registered with CTest individually so a failure names
# the exact obligation that broke.
set(RESOURCE_ENTITLEMENT_TESTS
    test_digest
    test_text_time
    test_canonical
    test_model
    test_store_lifecycle
    test_store_transfer
    test_store_fencing
    test_store_query
    test_durable
    test_corruption
    test_process
    test_property
    test_adversarial)

if(RESOURCE_ENTITLEMENT_BUILD_CLI)
    list(APPEND RESOURCE_ENTITLEMENT_TESTS test_cli)
endif()

foreach(test_name IN LISTS RESOURCE_ENTITLEMENT_TESTS)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE entl_test_support)
    resource_entitlement_strict_warnings(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Crash-consistency helper: terminates itself abruptly at a chosen commit stage.
add_executable(entl_crash_child crash_child.cpp)
target_link_libraries(entl_crash_child PRIVATE entl_test_support)
resource_entitlement_strict_warnings(entl_crash_child)

# Multiprocess helper used to prove writer-lock exclusion and kernel lock release.
add_executable(entl_holder_child holder_child.cpp)
target_link_libraries(entl_holder_child PRIVATE entl_test_support)
resource_entitlement_strict_warnings(entl_holder_child)

# AddressSanitizer on MSVC links against a dynamic runtime that is not on PATH by
# default. Test processes need the compiler's own binary directory.
set(ENTL_TEST_ENVIRONMENT "")
if(RESOURCE_ENTITLEMENT_ENABLE_ASAN AND MSVC)
    get_filename_component(ENTL_COMPILER_BINDIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    list(APPEND ENTL_TEST_ENVIRONMENT "PATH=${ENTL_COMPILER_BINDIR};$ENV{PATH}")
endif()

foreach(test_name IN LISTS RESOURCE_ENTITLEMENT_TESTS)
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${ENTL_TEST_ENVIRONMENT}")
endforeach()

list(APPEND ENTL_TEST_ENVIRONMENT
    "ENTL_CRASH_CHILD=$<TARGET_FILE:entl_crash_child>"
    "ENTL_HOLDER_CHILD=$<TARGET_FILE:entl_holder_child>")
set_tests_properties(test_process PROPERTIES ENVIRONMENT "${ENTL_TEST_ENVIRONMENT}")

if(RESOURCE_ENTITLEMENT_BUILD_CLI)
    list(APPEND ENTL_TEST_ENVIRONMENT "ENTL_CLI=$<TARGET_FILE:entl>")
    set_tests_properties(test_cli PROPERTIES ENVIRONMENT "${ENTL_TEST_ENVIRONMENT}")
endif()

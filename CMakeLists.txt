cmake_minimum_required(VERSION 3.25)

project(ResourceEntitlement
    VERSION 1.0.0
    DESCRIPTION "Generation-bound facility entitlement ledger with stale-authority fencing and deterministic verification"
    HOMEPAGE_URL "https://github.com/summonlabs/Resource-Entitlement"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(RESOURCE_ENTITLEMENT_IS_TOP_LEVEL ON)
else()
    set(RESOURCE_ENTITLEMENT_IS_TOP_LEVEL OFF)
endif()

option(RESOURCE_ENTITLEMENT_BUILD_CLI "Build the 'entl' command line tool." ON)
option(RESOURCE_ENTITLEMENT_BUILD_TESTS "Build the Resource Entitlement test suite." ${RESOURCE_ENTITLEMENT_IS_TOP_LEVEL})
option(RESOURCE_ENTITLEMENT_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors." ON)
option(RESOURCE_ENTITLEMENT_ENABLE_ASAN "Build with AddressSanitizer when the toolchain supports it." OFF)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# First-party strict warning policy.
#
# Applied per target (rather than through an exported INTERFACE target) so the
# installed package exports only real, self-contained imported targets.
# ---------------------------------------------------------------------------
function(resource_entitlement_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /Zc:inline /EHsc)
        if(RESOURCE_ENTITLEMENT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        if(RESOURCE_ENTITLEMENT_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address)
            target_link_options(${target} PRIVATE /INCREMENTAL:NO)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wundef -Wcast-qual -Wold-style-cast
            -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion -Wformat=2
            -Wconversion -Wsign-conversion -Wuseless-cast -Wzero-as-null-pointer-constant)
        if(RESOURCE_ENTITLEMENT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(RESOURCE_ENTITLEMENT_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(resource_entitlement
    src/version.cpp
    src/digest.cpp
    src/text.cpp
    src/time.cpp
    src/canonical.cpp
    src/model.cpp
    src/domain.cpp
    src/token.cpp
    src/fileio.cpp
    src/durable.cpp
    src/store.cpp
)
add_library(ResourceEntitlement::resource_entitlement ALIAS resource_entitlement)

target_include_directories(resource_entitlement
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

resource_entitlement_strict_warnings(resource_entitlement)
target_compile_features(resource_entitlement PUBLIC cxx_std_20)
set_target_properties(resource_entitlement PROPERTIES
    OUTPUT_NAME resource_entitlement
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

if(WIN32)
    target_compile_definitions(resource_entitlement PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# Command line interface
# ---------------------------------------------------------------------------
if(RESOURCE_ENTITLEMENT_BUILD_CLI)
    add_executable(entl
        cli/main.cpp
        cli/kv.cpp
        cli/json.cpp
        cli/commands.cpp
    )
    target_include_directories(entl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cli)
    target_link_libraries(entl PRIVATE resource_entitlement)
    resource_entitlement_strict_warnings(entl)
    set_target_properties(entl PROPERTIES OUTPUT_NAME entl)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS resource_entitlement
    EXPORT ResourceEntitlementTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(RESOURCE_ENTITLEMENT_BUILD_CLI)
    install(TARGETS entl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT ResourceEntitlementTargets
    NAMESPACE ResourceEntitlement::
    FILE ResourceEntitlementTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ResourceEntitlement)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ResourceEntitlementConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ResourceEntitlementConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ResourceEntitlement)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ResourceEntitlementConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ResourceEntitlementConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ResourceEntitlementConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ResourceEntitlement)

install(FILES LICENSE NOTICE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ResourceEntitlement)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(RESOURCE_ENTITLEMENT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

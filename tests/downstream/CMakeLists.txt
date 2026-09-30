cmake_minimum_required(VERSION 3.25)

# Independent out-of-tree consumer. It is deliberately not part of the
# repository build: it configures and builds against an installed
# ResourceEntitlement package through find_package(... CONFIG REQUIRED) only.
project(ResourceEntitlementConsumer
    VERSION 1.0.0
    DESCRIPTION "Out-of-tree consumer of the installed ResourceEntitlement package"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ResourceEntitlement CONFIG REQUIRED)

add_executable(entitlement_consumer consumer.cpp)
target_link_libraries(entitlement_consumer PRIVATE ResourceEntitlement::resource_entitlement)

if(MSVC)
    target_compile_options(entitlement_consumer PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(entitlement_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_test(NAME entitlement_consumer COMMAND entitlement_consumer)

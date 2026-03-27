cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(pubsub_itc_fw_project
    VERSION 0.1.0
    LANGUAGES CXX
)

# ------------------------------------------------------------
# Global C++ settings
# ------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(
    -g
    -O2
    -Wall
    -Wextra
    -pedantic
    -Wstrict-aliasing=2
    -Werror
)

# ------------------------------------------------------------
# Valgrind / TSAN / ASAN / Coverage
# ------------------------------------------------------------
option(ENABLE_VALGRIND "Build with Valgrind compatibility" OFF)
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if(ENABLE_VALGRIND)
    message(STATUS "Valgrind mode enabled")
    add_compile_definitions(USING_VALGRIND)
else()
    message(STATUS "Valgrind mode disabled")
    if(NOT ENABLE_TSAN)
        add_compile_options(-mcx16 -march=x86-64-v2)
    endif()
endif()

if(ENABLE_COVERAGE)
    message(STATUS "Building with code coverage instrumentation")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-O0 -g --coverage -fprofile-arcs -ftest-coverage -fno-inline -fno-inline-functions -fprofile-update=atomic)
        add_link_options(--coverage)
    else()
        message(FATAL_ERROR "Coverage enabled, but compiler does not support GCC/Clang coverage flags")
    endif()
else()
    add_definitions(-DENABLE_PERFORMANCE_TESTS)
endif()

if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
    add_compile_definitions(USING_VALGRIND)
endif()

# ------------------------------------------------------------
# Third-party discovery
# ------------------------------------------------------------
if (NOT DEFINED ENV{THIRDPARTY_DIR})
    message(FATAL_ERROR "THIRDPARTY_DIR environment variable is not set.")
endif()

set(THIRDPARTY_DIR $ENV{THIRDPARTY_DIR})
set(ABSEIL_VERSION $ENV{ABSEIL_VERSION})
set(PROTOBUF_VERSION $ENV{PROTOBUF_VERSION})
set(FMT_VERSION $ENV{FMT_VERSION})
set(QUILL_VERSION $ENV{QUILL_VERSION})
set(ARGPARSE_VERSION_FROM_ENV $ENV{ARGPARSE_VERSION})
set(GOOGLETEST_VERSION $ENV{GOOGLETEST_VERSION})

list(PREPEND CMAKE_PREFIX_PATH ${THIRDPARTY_DIR})
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules")

list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/abseil/${ABSEIL_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/protobuf/${PROTOBUF_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/fmt/${FMT_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/googletest/${GOOGLETEST_VERSION}")

find_package(Protobuf REQUIRED CONFIG)
find_package(Boost REQUIRED COMPONENTS headers filesystem)
find_package(fmt REQUIRED CONFIG)
find_package(quill REQUIRED)
find_package(argparse REQUIRED)
find_package(GTest REQUIRED CONFIG)

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(NUMA numa)
endif()

# ------------------------------------------------------------
# Pass proto file list to the library directory
# ------------------------------------------------------------
set(PUBSUB_ITC_FW_PROTO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/pubsub_itc_fw/include/pubsub_itc_fw/message_types.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/pubsub_itc_fw/include/pubsub_itc_fw/system_messages.proto
)

# ------------------------------------------------------------
# Add the real library (protobuf generation now happens inside it)
# ------------------------------------------------------------
add_subdirectory(libraries/pubsub_itc_fw)

# ------------------------------------------------------------
# Tests and performance harnesses
# ------------------------------------------------------------
add_subdirectory(libraries/pubsub_itc_fw/tests_common)
add_subdirectory(libraries/pubsub_itc_fw/tests)
add_subdirectory(libraries/pubsub_itc_fw/performance)

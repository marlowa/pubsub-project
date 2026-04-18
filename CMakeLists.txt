cmake_minimum_required(VERSION 3.11 FATAL_ERROR)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

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
        if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "11.0")
            add_compile_options(-mcx16 -march=x86-64-v2)
        else()
            # gcc 8.x does not support the x86-64-v2 march name.
            # Specify the individual features that matter for lock-free code.
            add_compile_options(-mcx16 -msse4.2 -mpopcnt)
        endif()
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
else()
    add_compile_options(-g -O2)
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
set(FMT_VERSION $ENV{FMT_VERSION})
set(QUILL_VERSION $ENV{QUILL_VERSION})
set(ARGPARSE_VERSION_FROM_ENV $ENV{ARGPARSE_VERSION})
set(GOOGLETEST_VERSION $ENV{GOOGLETEST_VERSION})
set(TOMLPLUSPLUS_VERSION $ENV{TOMLPLUSPLUS_VERSION})

list(PREPEND CMAKE_PREFIX_PATH ${THIRDPARTY_DIR})
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules")

list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/fmt/${FMT_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/quill/${QUILL_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/argparse/${ARGPARSE_VERSION_FROM_ENV}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/googletest/${GOOGLETEST_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/tomlplusplus/${TOMLPLUSPLUS_VERSION}")

# On RHEL/Rocky/CentOS cmake config files are installed under lib64/cmake
# rather than lib/cmake. Set _DIR explicitly for each package so
# find_package works on both Mint (lib) and RHEL8 (lib64).
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(LIB_SUFFIX "64")
else()
    set(LIB_SUFFIX "")
endif()

set(fmt_DIR
    "${THIRDPARTY_DIR}/installed/fmt/${FMT_VERSION}/lib${LIB_SUFFIX}/cmake/fmt")
set(quill_DIR
    "${THIRDPARTY_DIR}/installed/quill/${QUILL_VERSION}/lib${LIB_SUFFIX}/cmake/quill")
set(argparse_DIR
    "${THIRDPARTY_DIR}/installed/argparse/${ARGPARSE_VERSION_FROM_ENV}/lib${LIB_SUFFIX}/cmake/argparse")
set(GTest_DIR
    "${THIRDPARTY_DIR}/installed/googletest/${GOOGLETEST_VERSION}/lib${LIB_SUFFIX}/cmake/GTest")

message(STATUS "fmt_DIR:      ${fmt_DIR}")
message(STATUS "quill_DIR:    ${quill_DIR}")
message(STATUS "argparse_DIR: ${argparse_DIR}")
message(STATUS "GTest_DIR:    ${GTest_DIR}")

find_package(Boost REQUIRED COMPONENTS system filesystem)
find_package(fmt REQUIRED CONFIG)
find_package(quill REQUIRED CONFIG)
find_package(argparse REQUIRED CONFIG)
find_package(GTest REQUIRED CONFIG)

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(NUMA numa)
endif()

# ------------------------------------------------------------
# Add the library
# ------------------------------------------------------------
add_subdirectory(libraries/pubsub_itc_fw)

# ------------------------------------------------------------
# Tests and performance harnesses
# ------------------------------------------------------------
add_subdirectory(libraries/pubsub_itc_fw/tests_common)
add_subdirectory(libraries/pubsub_itc_fw/tests)
add_subdirectory(libraries/pubsub_itc_fw/performance)
add_subdirectory(libraries/pubsub_itc_fw/integration_tests)
add_subdirectory(applications)

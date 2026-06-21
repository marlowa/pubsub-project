cmake_minimum_required(VERSION 3.11 FATAL_ERROR)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(pubsub_itc_fw_project
    VERSION 0.1.0
    LANGUAGES CXX
)

# ------------------------------------------------------------
# GNU standard install directories (lib vs lib64 etc.)
# ------------------------------------------------------------
include(GNUInstallDirs)

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
    -Wnon-virtual-dtor
)

# -Wshadow is not enabled: the DSL code generator emits shared helper functions
# (write_uint64_le / read_uint64_le) using a loop variable named "i". These are
# top-level inline functions and do not shadow anything in current code, but
# enabling -Wshadow here should be re-evaluated after any DSL generator change.

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
            add_compile_options(-mcx16 -msse4.2 -mpopcnt)
        endif()
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
else()
    # -fno-omit-frame-pointer preserves frame pointers for perf/flamegraph
    # profiling without measurable impact on throughput at O2.
    add_compile_options(-g -O2 -fno-omit-frame-pointer)
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
# Python3
# ------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
message(STATUS "Python3: ${Python3_EXECUTABLE}")

# ------------------------------------------------------------
# Third-party discovery
# ------------------------------------------------------------
if(NOT DEFINED ENV{THIRDPARTY_DIR})
    message(FATAL_ERROR "THIRDPARTY_DIR environment variable is not set.")
endif()

set(THIRDPARTY_DIR $ENV{THIRDPARTY_DIR})
set(FMT_VERSION $ENV{FMT_VERSION})
set(QUILL_VERSION $ENV{QUILL_VERSION})
set(ARGPARSE_VERSION_FROM_ENV $ENV{ARGPARSE_VERSION})
set(GOOGLETEST_VERSION $ENV{GOOGLETEST_VERSION})
set(TOMLPLUSPLUS_VERSION $ENV{TOMLPLUSPLUS_VERSION})
set(ROBINMAP_VERSION $ENV{ROBINMAP_VERSION})

list(PREPEND CMAKE_PREFIX_PATH ${THIRDPARTY_DIR})
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules")

list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/fmt/${FMT_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/quill/${QUILL_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/argparse/${ARGPARSE_VERSION_FROM_ENV}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/googletest/${GOOGLETEST_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/tomlplusplus/${TOMLPLUSPLUS_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/tsl-robin-map/${ROBINMAP_VERSION}")

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
# tsl-robin-map is header-only; cmake installs its config under share/cmake, not lib/cmake.
set(tsl-robin-map_DIR
    "${THIRDPARTY_DIR}/installed/tsl-robin-map/${ROBINMAP_VERSION}/share/cmake/tsl-robin-map")

message(STATUS "fmt_DIR:          ${fmt_DIR}")
message(STATUS "quill_DIR:        ${quill_DIR}")
message(STATUS "argparse_DIR:     ${argparse_DIR}")
message(STATUS "GTest_DIR:        ${GTest_DIR}")
message(STATUS "tsl-robin-map_DIR: ${tsl-robin-map_DIR}")

find_package(Boost REQUIRED COMPONENTS system filesystem)
find_package(fmt REQUIRED CONFIG)
find_package(quill REQUIRED CONFIG)
find_package(argparse REQUIRED CONFIG)
find_package(GTest REQUIRED CONFIG)
find_package(tsl-robin-map REQUIRED CONFIG)
find_package(OpenSSL REQUIRED)

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(NUMA numa)
endif()

# ------------------------------------------------------------
# Doxygen target (runs at install time)
# ------------------------------------------------------------
option(ENABLE_DOXYGEN "Generate Doxygen documentation at install time" ON)

find_package(Doxygen QUIET)
if(DOXYGEN_FOUND AND ENABLE_DOXYGEN)
    set(DOXYGEN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/docs")

    add_custom_target(doxygen_docs
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DOXYGEN_OUTPUT_DIR}"
        COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_SOURCE_DIR}/Doxyfile"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )

    install(CODE "
        message(STATUS \"Running Doxygen...\")
        execute_process(
            COMMAND \"${CMAKE_COMMAND}\" --build \"${CMAKE_BINARY_DIR}\" --target doxygen_docs
            RESULT_VARIABLE _doxy_result
        )
        if(NOT _doxy_result EQUAL 0)
            message(WARNING \"Doxygen failed with exit code \${_doxy_result}\")
        endif()
    ")

    install(DIRECTORY "${DOXYGEN_OUTPUT_DIR}/html/"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}"
        OPTIONAL
    )
elseif(NOT DOXYGEN_FOUND)
    message(STATUS "Doxygen not found -- documentation will not be generated")
else()
    message(STATUS "Doxygen disabled (ENABLE_DOXYGEN=OFF)")
endif()

# ------------------------------------------------------------
# Python DSL generator install
# ------------------------------------------------------------
install(CODE "
    message(STATUS \"Installing Python DSL generator...\")
    execute_process(
        COMMAND \"${Python3_EXECUTABLE}\" -m pip install
            --prefix \"\${CMAKE_INSTALL_PREFIX}\"
            \"${CMAKE_SOURCE_DIR}/python\"
        RESULT_VARIABLE _pip_result
    )
    if(NOT _pip_result EQUAL 0)
        message(WARNING \"pip install failed with exit code \${_pip_result}\")
    endif()
")

# ------------------------------------------------------------
# DSL code generation
#
# Lifted out of applications/CMakeLists.txt so that targets in libraries/
# (such as the integration tests) can depend on the generated header in
# addition to the sample applications. Both sets of consumers add a
# dependency on fix_equity_orders_generated and include FIX_DSL_INCLUDE_DIR.
# Output lands in ${CMAKE_BINARY_DIR}/generated_dsl -- never in the source tree.
# ------------------------------------------------------------
set(FIX_DSL_SOURCE "${CMAKE_SOURCE_DIR}/applications/fix_equity_orders.dsl")
set(FIX_DSL_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated_dsl" CACHE INTERNAL "Where fix_equity_orders.hpp lives")
set(FIX_DSL_GENERATED_HPP "${FIX_DSL_INCLUDE_DIR}/fix_equity_orders.hpp")
set(FIX_DSL_GENERATOR "${CMAKE_SOURCE_DIR}/python/tools/generate_cpp_from_dsl.py")

add_custom_command(
    OUTPUT "${FIX_DSL_GENERATED_HPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DSL_GENERATOR}"
            --topics
            --namespace pubsub_itc_fw_app
            "${FIX_DSL_SOURCE}"
            --cpp "${FIX_DSL_GENERATED_HPP}"
    DEPENDS "${FIX_DSL_SOURCE}" "${FIX_DSL_GENERATOR}"
    COMMENT "Generating fix_equity_orders.hpp from fix_equity_orders.dsl"
    VERBATIM
)
add_custom_target(fix_equity_orders_generated ALL
    DEPENDS "${FIX_DSL_GENERATED_HPP}"
)

# Leader-follower protocol PDU header (no --topics: uses plain integer IDs 100-201).
set(LF_DSL_SOURCE "${CMAKE_SOURCE_DIR}/libraries/pubsub_itc_fw/include/pubsub_itc_fw/leader_follower.dsl")
set(LF_DSL_GENERATED_HPP "${FIX_DSL_INCLUDE_DIR}/leader_follower.hpp")

add_custom_command(
    OUTPUT "${LF_DSL_GENERATED_HPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DSL_GENERATOR}"
            --namespace pubsub_itc_fw_app
            "${LF_DSL_SOURCE}"
            --cpp "${LF_DSL_GENERATED_HPP}"
    DEPENDS "${LF_DSL_SOURCE}" "${FIX_DSL_GENERATOR}"
    COMMENT "Generating leader_follower.hpp from leader_follower.dsl"
    VERBATIM
)
add_custom_target(leader_follower_generated ALL
    DEPENDS "${LF_DSL_GENERATED_HPP}"
)

# Authentication service protocol PDU header (plain integer IDs 500-503; no --topics).
set(AUTH_DSL_SOURCE "${CMAKE_SOURCE_DIR}/applications/authentication.dsl")
set(AUTH_DSL_GENERATED_HPP "${FIX_DSL_INCLUDE_DIR}/authentication.hpp")

add_custom_command(
    OUTPUT "${AUTH_DSL_GENERATED_HPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DSL_GENERATOR}"
            --namespace pubsub_itc_fw_app
            "${AUTH_DSL_SOURCE}"
            --cpp "${AUTH_DSL_GENERATED_HPP}"
    DEPENDS "${AUTH_DSL_SOURCE}" "${FIX_DSL_GENERATOR}"
    COMMENT "Generating authentication.hpp from authentication.dsl"
    VERBATIM
)
add_custom_target(authentication_generated ALL
    DEPENDS "${AUTH_DSL_GENERATED_HPP}"
)

# Topic pub/sub protocol PDU header (plain integer IDs 107-111; no --topics).
set(TOPICS_DSL_SOURCE "${CMAKE_SOURCE_DIR}/applications/topics.dsl")
set(TOPICS_DSL_GENERATED_HPP "${FIX_DSL_INCLUDE_DIR}/topics.hpp")

add_custom_command(
    OUTPUT "${TOPICS_DSL_GENERATED_HPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DSL_GENERATOR}"
            --namespace pubsub_itc_fw_app
            "${TOPICS_DSL_SOURCE}"
            --cpp "${TOPICS_DSL_GENERATED_HPP}"
    DEPENDS "${TOPICS_DSL_SOURCE}" "${FIX_DSL_GENERATOR}"
    COMMENT "Generating topics.hpp from topics.dsl"
    VERBATIM
)
add_custom_target(topics_generated ALL
    DEPENDS "${TOPICS_DSL_GENERATED_HPP}"
)

# ------------------------------------------------------------
# Build info generation (version, git SHA/branch, build datetime)
# ------------------------------------------------------------
set(BUILD_INFO_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated_build_info")
set(BUILD_INFO_INCLUDE_DIR "${BUILD_INFO_OUTPUT_DIR}" CACHE INTERNAL "Directory containing generated BuildInfo.hpp")

add_custom_target(build_info_generated ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUILD_INFO_OUTPUT_DIR}/pubsub_itc_fw"
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DOUTPUT_DIR=${BUILD_INFO_OUTPUT_DIR}
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake
    COMMENT "Generating BuildInfo.hpp"
    VERBATIM
)

# ------------------------------------------------------------
# Add the libraries
# ------------------------------------------------------------
add_subdirectory(libraries/pubsub_itc_fw)
add_subdirectory(libraries/scram_crypto)

# ------------------------------------------------------------
# Tests and performance harnesses
# ------------------------------------------------------------
add_subdirectory(libraries/pubsub_itc_fw/tests_common)
add_subdirectory(libraries/pubsub_itc_fw/tests)
add_subdirectory(libraries/pubsub_itc_fw/performance)
add_subdirectory(libraries/pubsub_itc_fw/integration_tests)
add_subdirectory(applications)

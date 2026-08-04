cmake_minimum_required(VERSION 3.11 FATAL_ERROR)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(pubsub_itc_fw_project
    VERSION 0.3.0
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
# C++ coding-standards gate
# ------------------------------------------------------------
# Runs on EVERY build regardless of entry point -- build.sh/build.py, a bare
# `cmake --build`, or an IDE. check_standards.py scans the C++ sources (line
# length, naming, nullptr, etc.) and returns non-zero on any violation, which
# fails the build. build.py also runs it as a fast pre-flight, but wiring it
# into CMake here is what guarantees an IDE or direct cmake build cannot bypass
# the checks -- which is how over-length lines had slipped into the tree.
add_custom_target(check_standards ALL
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/check_standards.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Checking C++ coding standards (check_standards.py)"
    VERBATIM
)

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
set(PROMETHEUS_VERSION $ENV{PROMETHEUS_VERSION})

list(PREPEND CMAKE_PREFIX_PATH ${THIRDPARTY_DIR})
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules")

list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/fmt/${FMT_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/quill/${QUILL_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/argparse/${ARGPARSE_VERSION_FROM_ENV}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/googletest/${GOOGLETEST_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/tomlplusplus/${TOMLPLUSPLUS_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/tsl-robin-map/${ROBINMAP_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/prometheus-cpp/${PROMETHEUS_VERSION}")

# Each package prefix is on CMAKE_PREFIX_PATH above, and that is the whole mechanism.
# find_package(... REQUIRED CONFIG) already searches lib64, lib and share beneath a prefix and
# accepts either the <name>Config.cmake or the <name>-config.cmake spelling, so a third-party
# tree that is missing, is the wrong version, or was built for a different platform fails here
# at configure time, with CMake's own report of every path it tried for the named package.
find_package(fmt REQUIRED CONFIG)
find_package(quill REQUIRED CONFIG)
find_package(argparse REQUIRED CONFIG)
find_package(GTest REQUIRED CONFIG)
find_package(tsl-robin-map REQUIRED CONFIG)
# Brings in prometheus-cpp::core and ::pull (and ::push, unused). Its package config
# find_dependency()s Threads, ZLIB (the build has compression on) and CURL (push enabled),
# so those must be present on the host even though only the pull path is wanted here.
find_package(prometheus-cpp REQUIRED CONFIG)
find_package(OpenSSL REQUIRED)

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(NUMA numa)
endif()

# ------------------------------------------------------------
# Finding third-party shared libraries at run time
# ------------------------------------------------------------
# Third-party shared libraries stay where they were built, under THIRDPARTY_DIR, and are never
# copied next to our own binaries. Installed targets therefore need the path recorded in them:
# USE_LINK_PATH puts the directory of every linked library that lives outside the build tree
# into the installed target's run-time search path, so the set follows whatever was actually
# linked rather than a list maintained by hand.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# Emit DT_RPATH, not the DT_RUNPATH that current binutils default to. The two differ in
# precedence: DT_RUNPATH is consulted after LD_LIBRARY_PATH, DT_RPATH before it. A deployment
# host with a populated central library directory on LD_LIBRARY_PATH would otherwise supply its
# copy of a third-party library in preference to the one this build linked against, and a
# mismatch there surfaces as a missing symbol at start-up rather than anything nameable.
# add_link_options would be tidier but needs CMake 3.13; the declared minimum here is 3.11.
string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--disable-new-dtags")
string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--disable-new-dtags")

# ------------------------------------------------------------
# Doxygen target (runs at install time)
# ------------------------------------------------------------
option(ENABLE_DOXYGEN "Generate Doxygen documentation at install time" ON)

# On the RHEL8 family, graphviz/dot makes Doxygen extremely slow. Auto-detect that
# family so we can run Doxygen WITHOUT dot there (built-in class inheritance
# diagrams only), while Mint and other hosts keep the full dot graphs. Overridable
# via -DDOXYGEN_DISABLE_DOT=ON/OFF.
set(_pubsub_el8_family OFF)
if(EXISTS "/etc/os-release")
    file(READ "/etc/os-release" _pubsub_os_release)
    if(_pubsub_os_release MATCHES "(rhel|rocky|centos|almalinux)" AND
       _pubsub_os_release MATCHES "VERSION_ID=\"?8")
        set(_pubsub_el8_family ON)
    endif()
endif()
option(DOXYGEN_DISABLE_DOT
       "Run Doxygen without graphviz dot (much faster on RHEL8; disables dot graphs and the \\dotfile architecture map, keeps built-in class diagrams)"
       ${_pubsub_el8_family})

find_package(Doxygen QUIET)
if(DOXYGEN_FOUND AND ENABLE_DOXYGEN)
    set(DOXYGEN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/docs")

    # The committed Doxyfile hardcodes a RELATIVE "OUTPUT_DIRECTORY = build/docs", and this
    # target runs doxygen with WORKING_DIRECTORY = the source tree. Handed that file
    # directly, doxygen therefore writes to <source>/build/docs no matter what
    # CMAKE_BINARY_DIR is -- so a build configured elsewhere silently wrote its docs over
    # the default build tree's. That is how the Rocky container, which bind-mounts this
    # repo and runs as root, left 3488 root-owned files in the host's build/docs and broke
    # every subsequent host build with a permission error nowhere near the real cause.
    #
    # So the Doxyfile is never passed to doxygen as-is. A small generated wrapper @INCLUDEs
    # it and overrides OUTPUT_DIRECTORY with an absolute path; doxygen applies config lines
    # top to bottom and the last assignment of a key wins. The committed file stays valid
    # for a plain `doxygen Doxyfile` run by hand, which is why it is not simply edited.
    # build.py's run_doxygen() does the same thing by piping the config in on stdin.
    set(_pubsub_doxyfile "${CMAKE_BINARY_DIR}/Doxyfile.generated")
    set(_pubsub_doxyfile_overrides
        "# Generated by CMake -- do not edit. Overrides of the committed Doxyfile.\n"
        "@INCLUDE = ${CMAKE_SOURCE_DIR}/Doxyfile\n"
        "OUTPUT_DIRECTORY = ${DOXYGEN_OUTPUT_DIR}\n"
    )

    if(DOXYGEN_DISABLE_DOT)
        # HAVE_DOT=NO is all that is needed: Doxygen then draws class inheritance
        # diagrams with its built-in generator (the CLASS_DIAGRAMS default is YES),
        # and every dot-only graph is skipped. We deliberately do NOT set
        # CLASS_DIAGRAMS explicitly -- it is obsolete on doxygen 1.9+, and with
        # WARN_AS_ERROR=FAIL_ON_WARNINGS that would fail the build on newer hosts.
        # The extra GRAPH=NO lines are belt-and-braces (they are auto-off anyway).
        list(APPEND _pubsub_doxyfile_overrides
            "HAVE_DOT            = NO\n"
            "COLLABORATION_GRAPH = NO\n"
            "INCLUDE_GRAPH       = NO\n"
            "INCLUDED_BY_GRAPH   = NO\n"
            "GRAPHICAL_HIERARCHY = NO\n"
            "DIRECTORY_GRAPH     = NO\n"
        )
        # The architecture map is a \dotfile graph, so it is not rendered without
        # dot; its post-build link check only makes sense when dot is enabled.
        set(_pubsub_archmap_check
            COMMAND ${CMAKE_COMMAND} -E echo
                    "Skipping architecture-map check (dot disabled -- no map rendered)")
        message(STATUS "Doxygen: dot disabled (RHEL8-style fast docs); built-in class diagrams only")
    else()
        # Fail the build if any architecture-map box points at a bare directory.
        # doxygen 1.8.14 (RHEL8) does not fail on an unresolved \ref inside a dot
        # URL, so this catches dead map links that WARN_AS_ERROR would miss there.
        set(_pubsub_archmap_check
            COMMAND ${Python3_EXECUTABLE}
                    "${CMAKE_SOURCE_DIR}/scripts/check_architecture_map.py"
                    "${DOXYGEN_OUTPUT_DIR}/html")
    endif()

    string(JOIN "" _pubsub_doxyfile_content ${_pubsub_doxyfile_overrides})
    file(WRITE "${_pubsub_doxyfile}" "${_pubsub_doxyfile_content}")

    add_custom_target(doxygen_docs
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DOXYGEN_OUTPUT_DIR}"
        COMMAND ${DOXYGEN_EXECUTABLE} "${_pubsub_doxyfile}"
        ${_pubsub_archmap_check}
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
            # Fatal, not a warning. A warning here is how a documentation build stayed
            # broken unnoticed: it scrolls past in the middle of the install output.
            message(FATAL_ERROR \"Doxygen failed with exit code \${_doxy_result}\")
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
# dependency on fix_orders_generated and include FIX_DSL_INCLUDE_DIR.
# Output lands in ${CMAKE_BINARY_DIR}/generated_dsl -- never in the source tree.
# ------------------------------------------------------------
set(FIX_DSL_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated_dsl" CACHE INTERNAL "Where fix_orders.hpp lives")
set(FIX_DSL_GENERATED_HPP "${FIX_DSL_INCLUDE_DIR}/fix_orders.hpp")

# The serialisation .dsl is itself generated -- a build artifact, never a source file -- by
# expanding a curated FIX data dictionary. The committed inputs are the DD XML and the message
# list below. It is generated into the build tree (${FIX_DSL_INCLUDE_DIR}); applications/pubsub.dsl
# `include`s it by filename, resolved via the generator's --include-dir search path (below), so no
# generated file lives in the source tree. See docs/design/fix_pdu_generation.md.
set(FIX_DD_SOURCE "${CMAKE_SOURCE_DIR}/applications/fix_orders.dd.xml")
set(FIX_DSL_GENERATED "${FIX_DSL_INCLUDE_DIR}/fix_orders.dsl")
set(FIX_DD_GENERATOR "${CMAKE_SOURCE_DIR}/python/tools/generate_dd_to_dsl.py")
file(GLOB FIX_DD_GENERATOR_MODULES
    "${CMAKE_SOURCE_DIR}/python/dd_to_dsl/*.py"
    "${CMAKE_SOURCE_DIR}/python/fix_dictionary/*.py")

add_custom_command(
    OUTPUT "${FIX_DSL_GENERATED}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DD_GENERATOR}"
            --dd "${FIX_DD_SOURCE}"
            --message NewOrderSingle:1000
            --message OrderCancelRequest:1001
            --message ExecutionReport:1002
            --output "${FIX_DSL_GENERATED}"
    DEPENDS "${FIX_DD_SOURCE}" "${FIX_DD_GENERATOR}" ${FIX_DD_GENERATOR_MODULES}
    COMMENT "Generating fix_orders.dsl from the FIX data dictionary"
    VERBATIM
)

set(FIX_DSL_GENERATOR "${CMAKE_SOURCE_DIR}/python/tools/generate_cpp_from_dsl.py")
# The tool imports the python/dsl package; depend on those modules too so that a
# generator change (not just a .dsl change) regenerates the headers.
file(GLOB FIX_DSL_GENERATOR_MODULES "${CMAKE_SOURCE_DIR}/python/dsl/*.py")

add_custom_command(
    OUTPUT "${FIX_DSL_GENERATED_HPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DSL_GENERATOR}"
            --pdu-id-enum
            --namespace pubsub_itc_fw_app
            "${FIX_DSL_GENERATED}"
            --cpp "${FIX_DSL_GENERATED_HPP}"
    DEPENDS "${FIX_DSL_GENERATED}" "${FIX_DSL_GENERATOR}" ${FIX_DSL_GENERATOR_MODULES}
    COMMENT "Generating fix_orders.hpp from fix_orders.dsl"
    VERBATIM
)
add_custom_target(fix_orders_generated ALL
    DEPENDS "${FIX_DSL_GENERATED_HPP}"
)
# Run the coding-standards gate before code generation / compilation so a
# violation fails the build early rather than after a long C++ build.
add_dependencies(fix_orders_generated check_standards)

# Leader-follower protocol PDU header (no --pdu-id-enum: uses plain integer IDs 100-201).
set(LF_DSL_SOURCE "${CMAKE_SOURCE_DIR}/libraries/pubsub_itc_fw/include/pubsub_itc_fw/leader_follower.dsl")
set(LF_DSL_GENERATED_HPP "${FIX_DSL_INCLUDE_DIR}/leader_follower.hpp")

add_custom_command(
    OUTPUT "${LF_DSL_GENERATED_HPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DSL_GENERATOR}"
            --namespace pubsub_itc_fw_app
            "${LF_DSL_SOURCE}"
            --cpp "${LF_DSL_GENERATED_HPP}"
    DEPENDS "${LF_DSL_SOURCE}" "${FIX_DSL_GENERATOR}" ${FIX_DSL_GENERATOR_MODULES}
    COMMENT "Generating leader_follower.hpp from leader_follower.dsl"
    VERBATIM
)
add_custom_target(leader_follower_generated ALL
    DEPENDS "${LF_DSL_GENERATED_HPP}"
)

# Authentication service protocol PDU header (plain integer IDs 500-503; no --pdu-id-enum).
set(AUTH_DSL_SOURCE "${CMAKE_SOURCE_DIR}/applications/authentication.dsl")
set(AUTH_DSL_GENERATED_HPP "${FIX_DSL_INCLUDE_DIR}/authentication.hpp")

add_custom_command(
    OUTPUT "${AUTH_DSL_GENERATED_HPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DSL_GENERATOR}"
            --namespace pubsub_itc_fw_app
            "${AUTH_DSL_SOURCE}"
            --cpp "${AUTH_DSL_GENERATED_HPP}"
    DEPENDS "${AUTH_DSL_SOURCE}" "${FIX_DSL_GENERATOR}" ${FIX_DSL_GENERATOR_MODULES}
    COMMENT "Generating authentication.hpp from authentication.dsl"
    VERBATIM
)
add_custom_target(authentication_generated ALL
    DEPENDS "${AUTH_DSL_GENERATED_HPP}"
)

# Binary gateway session protocol PDU header (IDs 700-709; no --pdu-id-enum).
set(BINARY_SESSION_DSL_SOURCE "${CMAKE_SOURCE_DIR}/applications/binary_session.dsl")
set(BINARY_SESSION_DSL_GENERATED_HPP "${FIX_DSL_INCLUDE_DIR}/binary_session.hpp")

add_custom_command(
    OUTPUT "${BINARY_SESSION_DSL_GENERATED_HPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DSL_GENERATOR}"
            --namespace pubsub_itc_fw_app
            "${BINARY_SESSION_DSL_SOURCE}"
            --cpp "${BINARY_SESSION_DSL_GENERATED_HPP}"
    DEPENDS "${BINARY_SESSION_DSL_SOURCE}" "${FIX_DSL_GENERATOR}" ${FIX_DSL_GENERATOR_MODULES}
    COMMENT "Generating binary_session.hpp from binary_session.dsl"
    VERBATIM
)
add_custom_target(binary_session_generated ALL
    DEPENDS "${BINARY_SESSION_DSL_GENERATED_HPP}"
)

# Matching engine book replication protocol PDU header (IDs 600-619).
set(ME_REPL_DSL_SOURCE "${CMAKE_SOURCE_DIR}/applications/matching_engine/matching_engine_replication.dsl")
set(ME_REPL_DSL_GENERATED_HPP "${FIX_DSL_INCLUDE_DIR}/matching_engine_replication.hpp")

add_custom_command(
    OUTPUT "${ME_REPL_DSL_GENERATED_HPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DSL_GENERATOR}"
            --namespace pubsub_itc_fw_app
            "${ME_REPL_DSL_SOURCE}"
            --cpp "${ME_REPL_DSL_GENERATED_HPP}"
    DEPENDS "${ME_REPL_DSL_SOURCE}" "${FIX_DSL_GENERATOR}" ${FIX_DSL_GENERATOR_MODULES}
    COMMENT "Generating matching_engine_replication.hpp from matching_engine_replication.dsl"
    VERBATIM
)
add_custom_target(matching_engine_replication_generated ALL
    DEPENDS "${ME_REPL_DSL_GENERATED_HPP}"
)

# Topic pub/sub protocol PDU header (plain integer IDs 107-111; no --pdu-id-enum).
set(TOPICS_DSL_SOURCE "${CMAKE_SOURCE_DIR}/applications/topics.dsl")
set(TOPICS_DSL_GENERATED_HPP "${FIX_DSL_INCLUDE_DIR}/topics.hpp")

add_custom_command(
    OUTPUT "${TOPICS_DSL_GENERATED_HPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DSL_GENERATOR}"
            --namespace pubsub_itc_fw_app
            "${TOPICS_DSL_SOURCE}"
            --cpp "${TOPICS_DSL_GENERATED_HPP}"
    DEPENDS "${TOPICS_DSL_SOURCE}" "${FIX_DSL_GENERATOR}" ${FIX_DSL_GENERATOR_MODULES}
    COMMENT "Generating topics.hpp from topics.dsl"
    VERBATIM
)
add_custom_target(topics_generated ALL
    DEPENDS "${TOPICS_DSL_GENERATED_HPP}"
)

# Pub/sub topic catalog: the generated Topic enum + pdu-id/topic membership table
# (topics_registry.hpp) and the human-readable catalog (topics_catalog.md). Both
# are generated from the index applications/pubsub.dsl, which `include`s
# fix_orders.dsl for the message/pdu-id definitions. That include is resolved via
# --include-dir ${FIX_DSL_INCLUDE_DIR} (fix_orders.dsl is generated into the build tree),
# so the command depends on BOTH the index and the generated .dsl it includes.
set(PUBSUB_DSL_SOURCE "${CMAKE_SOURCE_DIR}/applications/pubsub.dsl")
set(PUBSUB_TOPICS_REGISTRY_HPP "${FIX_DSL_INCLUDE_DIR}/topics_registry.hpp")
set(PUBSUB_TOPICS_CATALOG_DIR "${CMAKE_BINARY_DIR}/generated_docs")
set(PUBSUB_TOPICS_CATALOG_MD "${PUBSUB_TOPICS_CATALOG_DIR}/topics_catalog.md")

add_custom_command(
    OUTPUT "${PUBSUB_TOPICS_REGISTRY_HPP}" "${PUBSUB_TOPICS_CATALOG_MD}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DSL_INCLUDE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PUBSUB_TOPICS_CATALOG_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DSL_GENERATOR}"
            --namespace pubsub_itc_fw_app
            --include-dir "${FIX_DSL_INCLUDE_DIR}"
            "${PUBSUB_DSL_SOURCE}"
            --topics-registry "${PUBSUB_TOPICS_REGISTRY_HPP}"
            --topics-catalog "${PUBSUB_TOPICS_CATALOG_MD}"
    DEPENDS "${PUBSUB_DSL_SOURCE}" "${FIX_DSL_GENERATED}" "${FIX_DSL_GENERATOR}" ${FIX_DSL_GENERATOR_MODULES}
    COMMENT "Generating topics_registry.hpp + topics_catalog.md from pubsub.dsl"
    VERBATIM
)
add_custom_target(topics_registry_generated ALL
    DEPENDS "${PUBSUB_TOPICS_REGISTRY_HPP}"
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
# FIX dictionary header generation (tag numbers, message types, enumerated
# values, tag->name table, and DATA/LENGTH field pairing) for the fix_codec
# library. Generated from the QuickFIX-style FIX XML dictionaries by the
# pure-Python generator; the header lands in the build tree under a fix_codec/
# subdir so consumers include it as <fix_codec/fix_dictionary.hpp>, matching the
# library's hand-written headers. Never written into the source tree.
# ------------------------------------------------------------
set(FIX_DICT_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated_fix" CACHE INTERNAL "Where fix_dictionary.hpp lives")
set(FIX_DICT_GENERATED_HPP "${FIX_DICT_INCLUDE_DIR}/fix_codec/fix_dictionary.hpp")
set(FIX_DICT_GENERATOR "${CMAKE_SOURCE_DIR}/python/tools/generate_fix_dictionary.py")
set(FIX_DICT_SOURCES
    "${CMAKE_SOURCE_DIR}/libraries/fix_codec/data_dictionary/FIXT11.xml"
    "${CMAKE_SOURCE_DIR}/libraries/fix_codec/data_dictionary/FIX50SP2.xml")
# Regenerate when the generator package modules change, not just the XML.
file(GLOB FIX_DICT_GENERATOR_MODULES "${CMAKE_SOURCE_DIR}/python/fix_dictionary/*.py")

add_custom_command(
    OUTPUT "${FIX_DICT_GENERATED_HPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIX_DICT_INCLUDE_DIR}/fix_codec"
    COMMAND ${Python3_EXECUTABLE} "${FIX_DICT_GENERATOR}"
            ${FIX_DICT_SOURCES}
            --namespace fix_codec
            --output "${FIX_DICT_GENERATED_HPP}"
    DEPENDS ${FIX_DICT_SOURCES} "${FIX_DICT_GENERATOR}" ${FIX_DICT_GENERATOR_MODULES}
    COMMENT "Generating fix_dictionary.hpp from FIX XML data dictionaries"
    VERBATIM
)
add_custom_target(fix_dictionary_generated ALL
    DEPENDS "${FIX_DICT_GENERATED_HPP}"
)
# Run the coding-standards gate before generation so a violation fails early.
add_dependencies(fix_dictionary_generated check_standards)

# ------------------------------------------------------------
# Add the libraries
# ------------------------------------------------------------
add_subdirectory(libraries/pubsub_itc_fw)
add_subdirectory(libraries/scram_crypto)
add_subdirectory(libraries/fix_codec)

# ------------------------------------------------------------
# Tests and performance harnesses
# ------------------------------------------------------------
add_subdirectory(libraries/pubsub_itc_fw/tests_common)
add_subdirectory(libraries/pubsub_itc_fw/tests)
add_subdirectory(libraries/pubsub_itc_fw/performance)
add_subdirectory(libraries/pubsub_itc_fw/integration_tests)
add_subdirectory(applications)

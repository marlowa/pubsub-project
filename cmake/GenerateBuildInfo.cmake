# cmake/GenerateBuildInfo.cmake
#
# Called via cmake -P at build time with:
#   -DSOURCE_DIR=<project root>
#   -DOUTPUT_DIR=<build dir>/generated_build_info
#   -DPROJECT_VERSION=<semver>
#
# Writes OUTPUT_DIR/pubsub_itc_fw/BuildInfo.hpp, using copy_if_different so
# an unchanged git state does not invalidate downstream compilation units.

find_program(GIT_EXECUTABLE git)

if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE git_result
    )
    if(NOT git_result EQUAL 0)
        set(GIT_SHA "")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE git_result
    )
    if(NOT git_result EQUAL 0)
        set(GIT_BRANCH "")
    endif()
endif()

if(NOT GIT_SHA)
    set(GIT_SHA "unknown")
endif()
if(NOT GIT_BRANCH)
    set(GIT_BRANCH "unknown")
endif()

string(TIMESTAMP BUILD_DATETIME "%Y-%m-%dT%H:%M:%SZ" UTC)

set(tmp_file "${OUTPUT_DIR}/pubsub_itc_fw/BuildInfo.hpp.tmp")
set(out_file "${OUTPUT_DIR}/pubsub_itc_fw/BuildInfo.hpp")

configure_file("${SOURCE_DIR}/cmake/BuildInfo.hpp.in" "${tmp_file}" @ONLY)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${tmp_file}" "${out_file}"
)

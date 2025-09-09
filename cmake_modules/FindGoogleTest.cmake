# FindGoogleTest.cmake
# Find GoogleTest library installation
#
# This module defines:
#  GOOGLETEST_FOUND - True if GoogleTest is found
#  GOOGLETEST_INCLUDE_DIRS - Include directories for GoogleTest
#  GOOGLETEST_LIBRARIES - Libraries to link against
#  GOOGLETEST_VERSION - Version of GoogleTest found
#
# This module also creates the following imported targets:
#  GTest::GTest - The main gtest library
#  GTest::Main - The gtest_main library
#  GMock::GMock - The gmock library
#  GMock::Main - The gmock_main library

# Use environment variable if set
if(DEFINED ENV{GOOGLETEST_VERSION})
    set(GOOGLETEST_VERSION $ENV{GOOGLETEST_VERSION})
else()
    set(GOOGLETEST_VERSION "1.8.1")
endif()

# Set search paths
if(DEFINED ENV{THIRDPARTY_DIR})
    set(GOOGLETEST_ROOT_DIR "$ENV{THIRDPARTY_DIR}/googletest/googletest-${GOOGLETEST_VERSION}")
else()
    message(FATAL_ERROR "THIRDPARTY_DIR environment variable not set")
endif()

# Find include directory
find_path(GOOGLETEST_INCLUDE_DIR
    NAMES gtest/gtest.h
    PATHS ${GOOGLETEST_ROOT_DIR}/include
    NO_DEFAULT_PATH
)

# Find libraries
find_library(GTEST_LIBRARY
    NAMES gtest
    PATHS ${GOOGLETEST_ROOT_DIR}/lib ${GOOGLETEST_ROOT_DIR}/lib64
    NO_DEFAULT_PATH
)

find_library(GTEST_MAIN_LIBRARY
    NAMES gtest_main
    PATHS ${GOOGLETEST_ROOT_DIR}/lib ${GOOGLETEST_ROOT_DIR}/lib64
    NO_DEFAULT_PATH
)

find_library(GMOCK_LIBRARY
    NAMES gmock
    PATHS ${GOOGLETEST_ROOT_DIR}/lib ${GOOGLETEST_ROOT_DIR}/lib64
    NO_DEFAULT_PATH
)

find_library(GMOCK_MAIN_LIBRARY
    NAMES gmock_main
    PATHS ${GOOGLETEST_ROOT_DIR}/lib ${GOOGLETEST_ROOT_DIR}/lib64
    NO_DEFAULT_PATH
)

# Handle standard arguments
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GoogleTest
    REQUIRED_VARS GOOGLETEST_INCLUDE_DIR GTEST_LIBRARY GTEST_MAIN_LIBRARY
    VERSION_VAR GOOGLETEST_VERSION
)

if(GOOGLETEST_FOUND)
    # Set traditional variables for compatibility with older CMake
    set(GOOGLETEST_INCLUDE_DIRS ${GOOGLETEST_INCLUDE_DIR})
    set(GOOGLETEST_LIBRARIES ${GTEST_LIBRARY} ${GTEST_MAIN_LIBRARY})

    # Add GMock libraries if found
    if(GMOCK_LIBRARY)
        list(APPEND GOOGLETEST_LIBRARIES ${GMOCK_LIBRARY})
    endif()

    if(GMOCK_MAIN_LIBRARY)
        list(APPEND GOOGLETEST_LIBRARIES ${GMOCK_MAIN_LIBRARY})
    endif()
endif()

# Mark variables as advanced
mark_as_advanced(
    GOOGLETEST_INCLUDE_DIR
    GTEST_LIBRARY
    GTEST_MAIN_LIBRARY
    GMOCK_LIBRARY
    GMOCK_MAIN_LIBRARY
)
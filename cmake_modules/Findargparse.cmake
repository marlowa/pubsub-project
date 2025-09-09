# FindArgparse.cmake
# Finds the argparse header-only library
# 
# This module defines:
#  argparse_FOUND - True if argparse is found
#  argparse::argparse - Imported target for argparse
#
# The user should set THIRDPARTY_DIR and ARGPARSE_VERSION environment variables

# Get the argparse version from environment variable
if (NOT DEFINED ENV{ARGPARSE_VERSION})
    message(FATAL_ERROR "ARGPARSE_VERSION environment variable is not set. Please set it to the exact version of argparse (e.g., '3.2').")
endif()

if (NOT DEFINED ENV{THIRDPARTY_DIR})
    message(FATAL_ERROR "THIRDPARTY_DIR environment variable is not set.")
endif()

set(ARGPARSE_VERSION_FROM_ENV $ENV{ARGPARSE_VERSION})
set(THIRDPARTY_ROOT_DIR $ENV{THIRDPARTY_DIR})

# Construct the expected path based on your naming convention
# Assuming structure: $THIRDPARTY_DIR/argparse/argparse-${VERSION}/include/
set(ARGPARSE_ROOT_HINT "${THIRDPARTY_ROOT_DIR}/argparse/argparse-${ARGPARSE_VERSION_FROM_ENV}")

# Alternative paths to search
set(ARGPARSE_SEARCH_PATHS
    "${ARGPARSE_ROOT_HINT}/include"
    "${ARGPARSE_ROOT_HINT}/single_include"
    "${THIRDPARTY_ROOT_DIR}/argparse/include"
    "${THIRDPARTY_ROOT_DIR}/argparse/single_include"
    "${THIRDPARTY_ROOT_DIR}/include"
)

# Find the header file
find_path(argparse_INCLUDE_DIR
    NAMES argparse/argparse.hpp
    PATHS ${ARGPARSE_SEARCH_PATHS}
    NO_DEFAULT_PATH
)

# Also try system paths if not found
if(NOT argparse_INCLUDE_DIR)
    find_path(argparse_INCLUDE_DIR
        NAMES argparse/argparse.hpp
    )
endif()

# Handle standard find_package arguments
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(argparse
    REQUIRED_VARS argparse_INCLUDE_DIR
    VERSION_VAR ARGPARSE_VERSION_FROM_ENV
)

# Create imported target if found
if(argparse_FOUND AND NOT TARGET argparse::argparse)
    add_library(argparse::argparse INTERFACE IMPORTED)
    set_target_properties(argparse::argparse PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${argparse_INCLUDE_DIR}"
    )
endif()

# Mark variables as advanced
mark_as_advanced(argparse_INCLUDE_DIR)

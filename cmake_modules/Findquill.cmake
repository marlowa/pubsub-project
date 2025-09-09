# Findquill.cmake
# Finds the quill header-only logging library
#
# This module defines:
#  quill_FOUND - True if quill is found
#  quill::quill - Imported target for quill
#
# The user should set THIRDPARTY_DIR and QUILL_VERSION environment variables

# Get the quill version from environment variable
if (NOT DEFINED ENV{QUILL_VERSION})
    message(FATAL_ERROR "QUILL_VERSION environment variable is not set. Please set it to the exact version of quill (e.g., '10.0.1').")
endif()

if (NOT DEFINED ENV{THIRDPARTY_DIR})
    message(FATAL_ERROR "THIRDPARTY_DIR environment variable is not set.")
endif()

set(QUILL_VERSION_FROM_ENV $ENV{QUILL_VERSION})
set(THIRDPARTY_ROOT_DIR $ENV{THIRDPARTY_DIR})

# Construct the expected path based on your naming convention
# Assuming structure: $THIRDPARTY_DIR/quill/quill-${VERSION}/include/
set(QUILL_ROOT_HINT "${THIRDPARTY_ROOT_DIR}/quill/quill-${QUILL_VERSION_FROM_ENV}")

# Alternative paths to search
set(QUILL_SEARCH_PATHS
    "${QUILL_ROOT_HINT}/include"
    "${THIRDPARTY_ROOT_DIR}/quill/include"
    "${THIRDPARTY_ROOT_DIR}/include"
)

# Find the header file
find_path(quill_INCLUDE_DIR
    NAMES quill/Logger.h
    PATHS ${QUILL_SEARCH_PATHS}
    NO_DEFAULT_PATH
)

# Also try system paths if not found
if(NOT quill_INCLUDE_DIR)
    find_path(quill_INCLUDE_DIR
        NAMES quill/Logger.h
    )
endif()

# Handle standard find_package arguments
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(quill
    REQUIRED_VARS quill_INCLUDE_DIR
    VERSION_VAR QUILL_VERSION_FROM_ENV
)

# Create imported target if found
if(quill_FOUND AND NOT TARGET quill::quill)
    add_library(quill::quill INTERFACE IMPORTED)
    set_target_properties(quill::quill PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${quill_INCLUDE_DIR}"
    )

    # Add any compile definitions that quill might need
    set_target_properties(quill::quill PROPERTIES
        INTERFACE_COMPILE_FEATURES "cxx_std_17"
    )
endif()

# Mark variables as advanced
mark_as_advanced(quill_INCLUDE_DIR)
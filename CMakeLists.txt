# Minimum required CMake version. Use 3.16 or higher for modern features.
cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(pubsub_itc_fw_project
    VERSION 0.1.0
    LANGUAGES CXX
)

# Set the C++ standard to C++17.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Ensure strict C++17

# --- Global Compiler Options ---
# Always use debug symbols (-g) and enable all standard warnings and treat warnings as error (-Wall -pedantic -Werror).
add_compile_options(-g -Wall -pedantic -Werror)

# --- Optional Valgrind Support ---
option(ENABLE_VALGRIND "Build with Valgrind compatibility (disables lock-free optimizations)" OFF)

if(ENABLE_VALGRIND)
    message(STATUS "Valgrind mode enabled - disabling lock-free atomic optimizations")
    add_compile_definitions(USING_VALGRIND)
    message(STATUS "  - USING_VALGRIND macro defined")
    message(STATUS "  - Omitting -mcx16 and -march=x86-64-v2 flags")
else()
    message(STATUS "Valgrind mode DISABLED - using lock-free atomic implementation")
    message(STATUS " - USING_VALGRIND macro NOT defined")
    message(STATUS " - Enabling -mcx16 and -march=x86-64-v2")
    # These atomic flags conflict with Valgrind, only use in normal builds
    add_compile_options(-mcx16 -march=x86-64-v2)
endif()

# --- Configure Third-Party Dependency Discovery ---
# CMake will search for packages within the paths specified in CMAKE_PREFIX_PATH.
# We will prepend the directory specified by the THIRDPARTY_DIR environment variable.
# This assumes third-party libraries are installed under:
# $THIRDPARTY_DIR/<component_name>/<component_name>-major.minor.fix/
# where CMake expects to find 'lib/cmake/<component_name>/' or 'share/<component_name>/cmake/'
# containing the *Config.cmake files.

# Check if THIRDPARTY_DIR environment variable is set
if (NOT DEFINED ENV{THIRDPARTY_DIR})
    message(FATAL_ERROR "THIRDPARTY_DIR environment variable is not set. Please set it to the root of the third-party installations.")
endif()

set(THIRDPARTY_DIR $ENV{THIRDPARTY_DIR})
set(ABSEIL_VERSION $ENV{ABSEIL_VERSION})
set(PROTOBUF_VERSION $ENV{PROTOBUF_VERSION})
set(FMT_VERSION $ENV{FMT_VERSION})
set(QUILL_VERSION $ENV{QUILL_VERSION})
set(ARGPARSE_VERSION_FROM_ENV $ENV{ARGPARSE_VERSION})

# Add the THIRDPARTY_DIR to CMAKE_PREFIX_PATH.
# This makes CMake look under <THIRDPARTY_DIR>/ for packages.
# When find_package(PackageName) is called, CMake automatically looks for
# <THIRDPARTY_DIR>/PackageName/ or <THIRDPARTY_DIR>/<anything>/PackageName/
# and within standard subdirectories like 'lib/cmake/<PackageName>/' or 'share/<PackageName>/cmake/'.
# Your convention <component_name>-major.minor.fix needs to be handled by how
# the third-party library itself generates its Config.cmake files or if
# specific hints are given.
list(PREPEND CMAKE_PREFIX_PATH ${THIRDPARTY_DIR})

# Add a custom module path for finding packages that don't provide Config files
# or need custom discovery logic (like argparse in this case).
# This tells CMake to look in 'cmake_modules' first for FindXXX.cmake files.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules")

# --- Find Third-Party Dependencies ---
# Ensure these libraries are installed on your system or provided via vcpkg/conan
# or as submodules. CMake's find_package will look in standard locations
# and now, crucially, within the paths added to CMAKE_PREFIX_PATH.

list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/abseil/${ABSEIL_VERSION}")
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/protobuf/${PROTOBUF_VERSION}")

# Find Protobuf. This is crucial for generating C++ code from .proto files.
# For Protobuf, ensure its installation places `protobuf-config.cmake` or
# `ProtobufConfig.cmake` under `$THIRDPARTY_DIR/protobuf/protobuf-MAJOR.MINOR.PATCH/lib/cmake/protobuf/`
# or similar standard CMake package config locations.
find_package(Protobuf REQUIRED CONFIG)

# Boost usually works well with CMAKE_PREFIX_PATH if its installation is standard.
# It looks for `BoostConfig.cmake` or `boost-config.cmake`.
find_package(Boost REQUIRED COMPONENTS headers filesystem)

# Find fmt library for formatting.
# fmt needs `fmtConfig.cmake` or `fmt-config.cmake`.
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/fmt/${FMT_VERSION}")
find_package(fmt REQUIRED CONFIG)

# Find quill for logging. Quill typically requires its own build or a specific
# FindQuill.cmake module if not installed in a standard path.
# Assuming a standard install where `quillConfig.cmake` is findable.
find_package(quill REQUIRED)

# Find argparse.
# We are now providing a custom Findargparse.cmake in cmake_modules/,
# so simply calling find_package without 'CONFIG' will allow CMake to find it.
if (NOT DEFINED ENV{ARGPARSE_VERSION})
    message(FATAL_ERROR "ARGPARSE_VERSION environment variable is not set. Please set it to the exact version of argparse (e.g., '3.2').")
endif()

find_package(argparse REQUIRED)

set(GOOGLETEST_VERSION $ENV{GOOGLETEST_VERSION})
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_DIR}/installed/googletest/${GOOGLETEST_VERSION}")
find_package(GTest REQUIRED CONFIG)

# Find NUMA library
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(NUMA numa)
endif()

# --- Define the Protobuf Message Type ---
# This step tells CMake to compile the .proto files into .pb.h and .pb.cc
# The generated files will be placed in CMAKE_CURRENT_BINARY_DIR.
set(PUBSUB_ITC_FW_PROTO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/pubsub_itc_fw/include/pubsub_itc_fw/message_types.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/pubsub_itc_fw/include/pubsub_itc_fw/system_messages.proto
# Add other application-specific .proto files here as your project grows
)

# Create the pubsub_itc_fw directory in build folder for generated headers
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw)

# Custom protoc command to generate files in the right directory structure
add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/message_types.pb.cc
        ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/message_types.pb.h
        ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/system_messages.pb.cc
        ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/system_messages.pb.h
    COMMAND protobuf::protoc
        --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/libraries/pubsub_itc_fw/include
        ${PUBSUB_ITC_FW_PROTO_FILES}
    DEPENDS ${PUBSUB_ITC_FW_PROTO_FILES}
    COMMENT "Generating C++ protobuf files"
)

# Set the generated source and header files
set(PROTO_SRCS
    ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/message_types.pb.cc
    ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/system_messages.pb.cc
)
set(PROTO_HDRS
    ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/message_types.pb.h
    ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/system_messages.pb.h
)

# --- Define Your Library ---
# Create the 'pubsub_itc_fw_lib' library.
# We explicitly list the generated .pb.cc files
add_library(pubsub_itc_fw_lib
    ${PROTO_SRCS} # Generated .pb.cc files
    libraries/pubsub_itc_fw/src/ApplicationThread.cpp
    libraries/pubsub_itc_fw/src/InetAddress.cpp
    libraries/pubsub_itc_fw/src/EventMessage.cpp
    libraries/pubsub_itc_fw/src/Message.cpp
    libraries/pubsub_itc_fw/src/EventDistributor.cpp
    libraries/pubsub_itc_fw/src/ExpandablePoolAllocator.cpp
    libraries/pubsub_itc_fw/src/MillisecondClock.cpp
    libraries/pubsub_itc_fw/src/QuillLogger.cpp
    libraries/pubsub_itc_fw/src/Reactor.cpp
    libraries/pubsub_itc_fw/src/StringUtils.cpp
    libraries/pubsub_itc_fw/src/TcpAcceptor.cpp
    libraries/pubsub_itc_fw/src/TcpConnector.cpp
    libraries/pubsub_itc_fw/src/TcpSocket.cpp
)

# PUBLIC makes them available to targets that link against this library.
# $<BUILD_INTERFACE:...> for when building the project.
# $<INSTALL_INTERFACE:...> for when the library is installed.
target_include_directories(pubsub_itc_fw_lib PUBLIC
     # Include the directory where generated .pb.h files reside
     $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
     # Include your project's public headers under libraries
     $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libraries/pubsub_itc_fw/include>
     # When installed, the headers will be copied to CMAKE_INSTALL_INCLUDEDIR
     $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# PRIVATE means these are only needed for building this library.
target_link_libraries(pubsub_itc_fw_lib PUBLIC
     protobuf::libprotobuf
     Boost::headers        # Link against Boost headers
     fmt::fmt              # Link against fmt library
     quill::quill          # Link against quill
     argparse::argparse    # Link against argparse
     atomic
)

add_subdirectory(libraries/pubsub_itc_fw/tests_common)

# --- Add Test Subdirectory ---
# This includes the CMakeLists.txt file in the 'test' directory,
# allowing it to define and build the GTest executables.
add_subdirectory(libraries/pubsub_itc_fw/tests)

# Optional: Install the library and headers
# install(TARGETS pubsub_itc_fw_lib DESTINATION lib)
# install(DIRECTORY include/pubsub_itc_fw/ DESTINATION include)

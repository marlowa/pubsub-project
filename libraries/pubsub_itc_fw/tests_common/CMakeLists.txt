# CMakeLists.txt for pubsub_itc_fw library unit test helper library which includes a central test runner.

# CMakeLists.txt for pubsub_itc_fw library unit test helper library.

# Define a static library named 'pubsub_itc_fw_test_helpers'.
# This library will contain our custom 'main' function provided by TestRunner.cpp.
add_library(pubsub_itc_fw_test_helpers STATIC
    src/TestRunner.cpp # This file contains your custom 'main' function.
    src/UnitTestLogger.cpp
)

# Set the public include directories for this helper library.
# Any target linking to 'pubsub_itc_fw_test_helpers' will automatically inherit these include paths.
target_include_directories(pubsub_itc_fw_test_helpers PUBLIC
    # Include our helper headers (e.g., TestRunner.hpp)
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    # Include the main pubsub_itc_fw library headers (e.g., StringUtils.hpp)
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    ${GOOGLETEST_INCLUDE_DIRS}                                # Google Test headers
)

# Link this helper library against its dependencies. That also deals with include directories.
target_link_libraries(pubsub_itc_fw_test_helpers PRIVATE
     fmt::fmt              # Link against fmt library
     quill::quill
   ${GTEST_LIBRARY}
)

# Apply common compiler flags consistent with the rest of the project.
target_compile_options(pubsub_itc_fw_test_helpers PRIVATE -g -Wall -pedantic)

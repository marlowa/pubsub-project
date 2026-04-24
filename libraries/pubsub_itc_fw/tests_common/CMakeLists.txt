# Define a static library named 'pubsub_itc_fw_test_helpers'.
# This library will contain our custom 'main' function provided by TestRunner.cpp.
add_library(pubsub_itc_fw_test_helpers STATIC
    src/TestRunner.cpp
    src/UnitTestLogger.cpp
)

target_include_directories(pubsub_itc_fw_test_helpers PUBLIC
    # Include our helper headers (e.g., TestRunner.hpp)
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    # Include the main pubsub_itc_fw library headers (e.g., StringUtils.hpp)
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    ${THIRDPARTY_DIR}/installed/googletest/${GOOGLETEST_VERSION}/include
)

# Link this helper library against its dependencies. That also deals with include directories.
target_link_libraries(pubsub_itc_fw_test_helpers PRIVATE
     fmt::fmt              # Link against fmt library
     quill::quill
     ${GOOGLETEST_LIBRARIES} # Google Test framework (from your FindGoogleTest.cmake)
)

# Apply common compiler flags consistent with the rest of the project.
target_compile_options(pubsub_itc_fw_test_helpers PRIVATE -g -Wall -pedantic)

# CMakeLists.txt for pubsub_itc_fw library unit tests

# Create the test executable for pubsub_itc_fw library
add_executable(pubsub_itc_fw_tests
    ExpandablePoolAllocatorTest.cpp
    LoggerTest.cpp
    StringUtilsTest.cpp
    # Add other test files here as you create them
    # MessageTest.cpp
)

# Link against the library being tested
target_link_libraries(pubsub_itc_fw_tests PRIVATE
    pubsub_itc_fw_lib       # The library we're testing
    pubsub_itc_fw_test_helpers  # <--- Link to the custom test runner library
    ${GOOGLETEST_LIBRARIES} # Google Test framework (from your FindGoogleTest.cmake)
    Boost::filesystem       # Needed for boost::filesystem in tests
)

# Include directories - the test needs access to the same headers
target_include_directories(pubsub_itc_fw_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include  # Library headers
    ${CMAKE_CURRENT_BINARY_DIR}/../../..    # Generated protobuf headers
    ${GOOGLETEST_INCLUDE_DIRS}              # Google Test headers (from your FindGoogleTest.cmake)
)

# Add compiler flags consistent with the main project
target_compile_options(pubsub_itc_fw_tests PRIVATE -g -Wall -pedantic)

# Register the test with CTest so you can run it with 'make test' or 'ctest'
include(GoogleTest)
gtest_discover_tests(pubsub_itc_fw_tests)

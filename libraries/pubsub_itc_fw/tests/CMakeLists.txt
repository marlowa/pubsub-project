# CMakeLists.txt for pubsub_itc_fw library unit tests

# Create the test executable for pubsub_itc_fw library
add_executable(pubsub_itc_fw_tests
    ExpandablePoolAllocatorTest.cpp
    NumaAwarePoolAllocatorTest.cpp
    LoggerTest.cpp
    StringUtilsTest.cpp
    ThreadWithJoinTimeoutTest.cpp
    # Add other test files here as you create them
    # MessageTest.cpp
)

# Link against the library being tested
target_link_libraries(pubsub_itc_fw_tests PRIVATE
    pubsub_itc_fw_lib       # The library we're testing
    pubsub_itc_fw_test_helpers  # <--- Link to the custom test runner library
    GTest::gtest_main
    Boost::filesystem       # Needed for boost::filesystem in tests
)

# Include directories - the test needs access to the same headers
target_include_directories(pubsub_itc_fw_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include  # Library headers
    ${CMAKE_CURRENT_BINARY_DIR}/../../..    # Generated protobuf headers
    ${GOOGLETEST_INCLUDE_DIRS}              # Google Test headers (from your FindGoogleTest.cmake)
)

# Add NUMA if available
if(NUMA_FOUND)
    target_compile_definitions(pubsub_itc_fw_tests PRIVATE HAVE_NUMA)
    target_link_libraries(pubsub_itc_fw_tests PRIVATE ${NUMA_LIBRARIES})
    target_include_directories(pubsub_itc_fw_tests PRIVATE ${NUMA_INCLUDE_DIRS})
    message(STATUS "NUMA support enabled for tests")
else()
    message(STATUS "NUMA library not found - NUMA tests will be skipped")
endif()


# Add compiler flags consistent with the main project
target_compile_options(pubsub_itc_fw_tests PRIVATE -g -Wall -pedantic)

# Register the test with CTest so you can run it with 'make test' or 'ctest'
include(GoogleTest)
gtest_discover_tests(pubsub_itc_fw_tests)

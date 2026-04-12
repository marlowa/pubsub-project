# CMakeLists.txt for pubsub_itc_fw integration tests
#
# Integration tests use real loopback sockets and run two reactors
# simultaneously on separate threads. They are kept separate from unit
# tests because they exercise the full stack end-to-end and are slower.
#
# Disabled under sanitizers to avoid false positives from multi-threaded socket interaction.

if(ENABLE_ASAN OR ENABLE_TSAN)
    message(STATUS "Integration tests disabled (coverage/sanitizer build)")
    return()
endif()

message(STATUS "Building integration tests for pubsub_itc_fw")

add_executable(pubsub_itc_fw_integration_tests
    StatusQueryResponseTest.cpp
    VariableLengthPduTest.cpp
    RawBytesProtocolHandlerTest.cpp
)

# Ensure DSL headers are generated before building.
add_dependencies(pubsub_itc_fw_integration_tests
    pubsub_itc_fw_dsl
)

target_link_libraries(pubsub_itc_fw_integration_tests PRIVATE
    pubsub_itc_fw
    pubsub_itc_fw_test_helpers
    GTest::gtest_main
    Boost::filesystem
)

target_include_directories(pubsub_itc_fw_integration_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_BINARY_DIR}/../pubsub_itc_fw/pubsub_itc_fw
    ${CMAKE_CURRENT_BINARY_DIR}/../pubsub_itc_fw/dsl
)

include(GoogleTest)
gtest_discover_tests(pubsub_itc_fw_integration_tests
    PROPERTIES DEPENDS pubsub_itc_fw_tests
)

message(STATUS "  Added integration test: StatusQueryResponseTest")
message(STATUS "  Added integration test: VariableLengthPduTest")

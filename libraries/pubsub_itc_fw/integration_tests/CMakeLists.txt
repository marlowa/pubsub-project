# CMakeLists.txt for pubsub_itc_fw integration tests

if(ENABLE_ASAN OR ENABLE_TSAN)
    message(STATUS "Integration tests disabled (coverage/sanitizer build)")
    return()
endif()

message(STATUS "Building integration tests for pubsub_itc_fw")

add_executable(pubsub_itc_fw_integration_tests
    PduBurstIntegrationTest.cpp
    PduProtocolHandlerIntegrationTest.cpp
    StatusQueryResponseTest.cpp
    VariableLengthPduTest.cpp
    RawBytesProtocolHandlerIntegrationTest.cpp
)

add_dependencies(pubsub_itc_fw_integration_tests
    pubsub_itc_fw_dsl
    fix_equity_orders_generated
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
    ${FIX_DSL_INCLUDE_DIR}
)

include(GoogleTest)
gtest_discover_tests(pubsub_itc_fw_integration_tests
    PROPERTIES DEPENDS pubsub_itc_fw_tests
)

message(STATUS "  Added integration test: PduBurstIntegrationTest")
message(STATUS "  Added integration test: StatusQueryResponseTest")
message(STATUS "  Added integration test: VariableLengthPduTest")
message(STATUS "  Added integration test: RawBytesProtocolHandlerIntegrationTest")

# Install integration test executable -> bin
install(TARGETS pubsub_itc_fw_integration_tests
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

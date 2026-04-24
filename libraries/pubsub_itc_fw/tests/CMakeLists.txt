# CMakeLists.txt for pubsub_itc_fw library unit tests

add_executable(pubsub_itc_fw_tests
    ApplicationThreadTest.cpp
    BumpAllocatorTest.cpp
    EventMessageTest.cpp
    ExpandablePoolAllocatorTest.cpp
    ExpandableSlabAllocatorTest.cpp
    FixedSizeMemoryPoolTest.cpp
    NumaAwarePoolAllocatorTest.cpp
    LockFreeMessageQueueTest.cpp
    MillisecondClockTest.cpp
    PduFramerParserTest.cpp
    PduProtocolHandlerTest.cpp
    QuillLoggerTest.cpp
    ReactorTest.cpp
    ServiceRegistryTest.cpp
    SlabAllocatorTest.cpp
    SlabAllocatorAdversarialTest.cpp
    StringUtilsTest.cpp
    TcpAcceptorTest.cpp
    TcpSocketTest.cpp
    ThreadWithJoinTimeoutTest.cpp
    TomlConfigurationTest.cpp
    MirroredBufferTest.cpp
    InetAddressTest.cpp
    TcpConnectorTest.cpp
    OutboundConnectionTest.cpp
)

target_link_libraries(pubsub_itc_fw_tests PRIVATE
    pubsub_itc_fw
    pubsub_itc_fw_test_helpers
    GTest::gtest_main
    Boost::filesystem
)

target_include_directories(pubsub_itc_fw_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

if(NUMA_FOUND)
    target_compile_definitions(pubsub_itc_fw_tests PRIVATE HAVE_NUMA)
    target_link_libraries(pubsub_itc_fw_tests PRIVATE ${NUMA_LIBRARIES})
    target_include_directories(pubsub_itc_fw_tests PRIVATE ${NUMA_INCLUDE_DIRS})
    message(STATUS "NUMA support enabled for tests")
else()
    message(STATUS "NUMA library not found - NUMA tests will be skipped")
endif()

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(pubsub_itc_fw_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(pubsub_itc_fw_tests)
endif()

# Install test executable -> bin
install(TARGETS pubsub_itc_fw_tests
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

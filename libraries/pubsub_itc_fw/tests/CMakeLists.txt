# CMakeLists.txt for pubsub_itc_fw library unit tests

add_executable(pubsub_itc_fw_tests
    ApplicationAnnouncerTest.cpp
    ApplicationThreadTest.cpp
    BumpAllocatorTest.cpp
    CpuLayoutTest.cpp
    CpuPinningTest.cpp
    CpuRegistryTest.cpp
    ExternalWalSubscriberRegistryTest.cpp
    Crc32Test.cpp
    MetricKeyTest.cpp
    PrometheusEndpointTest.cpp
    EmptySlabQueueTest.cpp
    EventMessageTest.cpp
    ExpandablePoolAllocatorTest.cpp
    ExpandableSlabAllocatorTest.cpp
    FileLockTest.cpp
    FixedSizeMemoryPoolTest.cpp
    FwLogLevelTest.cpp
    GrowthReportingAllocatorTest.cpp
    HighResolutionClockTest.cpp
    IncrementalRehashMapTest.cpp
    InetAddressTest.cpp
    LockFreeMessageQueueTest.cpp
    LoggingConfigurationLoaderTest.cpp
    MappedSlotStoreTest.cpp
    MillisecondClockTest.cpp
    MirroredBufferTest.cpp
    NumaAwarePoolAllocatorTest.cpp
    OutboundConnectionTest.cpp
    PduFramerParserTest.cpp
    PduParserTest.cpp
    PduProtocolHandlerTest.cpp
    ProtocolHandlerInterfaceTest.cpp
    QuillLoggerTest.cpp
    ReactorTest.cpp
    ServiceRegistryTest.cpp
    SlabAllocatorTest.cpp
    StringUtilsTest.cpp
    TcpAcceptorTest.cpp
    TcpConnectorTest.cpp
    TcpSocketTest.cpp
    TlsContextTest.cpp
    TlsStateTest.cpp
    ThreadWithJoinTimeoutTest.cpp
    TimerTest.cpp
    TomlConfigurationTest.cpp
    WalCursorTest.cpp
    WalTest.cpp
    WalWriterReaderTest.cpp
    WallClockTest.cpp
)

target_link_libraries(pubsub_itc_fw_tests PRIVATE
    pubsub_itc_fw
    pubsub_itc_fw_test_helpers
    GTest::gtest_main
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

add_executable(fix_order_gateway
        FixCapture.cpp
        FixErEncoder.cpp
        FixOrderGatewayConfigurationLoader.cpp
        FixOrderGatewayThread.cpp
        FixParser.cpp
        FixSerialiser.cpp
        FixOrderGateway.cpp
)

target_include_directories(fix_order_gateway PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(fix_order_gateway PRIVATE
        pubsub_itc_fw
        scram_crypto
        fix_codec
        tsl::robin_map
)

# fix_codec is header-only; its generated dictionary header must exist before the
# gateway compiles. The migration onto fix_codec (inbound parse + validate) is
# documented in docs/venue/fix_order_gateway.md.
add_dependencies(fix_order_gateway fix_orders_generated authentication_generated fix_dictionary_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(fix_order_gateway PRIVATE
        FixCapture.hpp
        FixErEncoder.hpp
        FixOrderGatewayConfiguration.hpp
        FixOrderGatewayConfigurationLoader.hpp
        FixOrderGatewayThread.hpp
        FixMessage.hpp
        FixParser.hpp
        FixSerialiser.hpp
        FixSession.hpp
        FixOrderGateway.hpp
)

install(TARGETS fix_order_gateway
        RUNTIME DESTINATION bin
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/fix_order_gateway_a.toml
              ${CMAKE_CURRENT_SOURCE_DIR}/fix_order_gateway_b.toml
        DESTINATION etc/fix_order_gateway
)

# ── Unit tests ────────────────────────────────────────────────────────────────
add_executable(fix_order_gateway_tests
        tests/OpenOrderRemovalTest.cpp
        tests/FixCaptureTest.cpp
        tests/FixParserTest.cpp
        tests/FixSerialiserTest.cpp
        tests/FixErEncoderTest.cpp
        tests/GatewayMetricsTest.cpp
        tests/PoolMetricsReporterTest.cpp
        FixCapture.cpp
        FixParser.cpp
        FixSerialiser.cpp
        FixErEncoder.cpp
)

target_include_directories(fix_order_gateway_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(fix_order_gateway_tests PRIVATE
        pubsub_itc_fw
        pubsub_itc_fw_test_helpers
        fix_codec
        GTest::gtest_main
)

add_dependencies(fix_order_gateway_tests fix_dictionary_generated fix_orders_generated)

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(fix_order_gateway_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(fix_order_gateway_tests)
endif()

install(TARGETS fix_order_gateway_tests
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

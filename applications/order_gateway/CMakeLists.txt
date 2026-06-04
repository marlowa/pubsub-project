add_executable(order_gateway
        FixCapture.cpp
        FixErEncoder.cpp
        OrderGatewayConfigurationLoader.cpp
        OrderGatewayThread.cpp
        FixParser.cpp
        FixSerialiser.cpp
        OrderGateway.cpp
)

target_include_directories(order_gateway PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(order_gateway PRIVATE
        pubsub_itc_fw
        scram_crypto
)

add_dependencies(order_gateway fix_equity_orders_generated authentication_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(order_gateway PRIVATE
        FixCapture.hpp
        FixErEncoder.hpp
        OrderGatewayConfiguration.hpp
        OrderGatewayConfigurationLoader.hpp
        OrderGatewayThread.hpp
        FixMessage.hpp
        FixParser.hpp
        FixSerialiser.hpp
        FixSession.hpp
        OrderGateway.hpp
)

install(TARGETS order_gateway
        RUNTIME DESTINATION bin
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/order_gateway.toml
        DESTINATION etc/order_gateway
)

# ── Unit tests ────────────────────────────────────────────────────────────────
add_executable(order_gateway_tests
        tests/FixCaptureTest.cpp
        FixCapture.cpp
)

target_include_directories(order_gateway_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(order_gateway_tests PRIVATE
        pubsub_itc_fw
        pubsub_itc_fw_test_helpers
        GTest::gtest_main
)

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(order_gateway_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(order_gateway_tests)
endif()

install(TARGETS order_gateway_tests
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

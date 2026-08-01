add_executable(binary_order_gateway
        BinaryOrderGatewayConfigurationLoader.cpp
        BinaryOrderGatewayThread.cpp
        BinaryOrderGateway.cpp
)

target_include_directories(binary_order_gateway PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(binary_order_gateway PRIVATE
        pubsub_itc_fw
        scram_crypto
        tsl::robin_map
)

# No fix_codec or fix_dictionary dependency, unlike the FIX order gateway: clients speak the
# generated PDUs directly, so there is no FIX text to parse or validate here.
add_dependencies(binary_order_gateway fix_orders_generated binary_session_generated authentication_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(binary_order_gateway PRIVATE
        BinaryOrderGateway.hpp
        BinaryOrderGatewayConfiguration.hpp
        BinaryOrderGatewayConfigurationLoader.hpp
        BinaryOrderGatewayThread.hpp
        BinarySession.hpp
)

install(TARGETS binary_order_gateway
        RUNTIME DESTINATION bin
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/binary_order_gateway_a.toml
              ${CMAKE_CURRENT_SOURCE_DIR}/binary_order_gateway_b.toml
        DESTINATION etc/binary_order_gateway
)

# ── Reference client ──────────────────────────────────────────────────────────
# Drives the gateway end to end against the live pipeline, and doubles as the
# worked example of what a binary client must do. It uses plain sockets and the
# generated codecs rather than the framework, to show that a client needs nothing
# from pubsub_itc_fw but the PDU header layout.
add_executable(binary_client
        tools/BinaryClientMain.cpp
)

target_include_directories(binary_client PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(binary_client PRIVATE
        pubsub_itc_fw
)

add_dependencies(binary_client fix_orders_generated binary_session_generated)

install(TARGETS binary_client
        RUNTIME DESTINATION bin
)

# ── Load generator ────────────────────────────────────────────────────────────
# The counterpart of fix8's f8test, which perf_run.py drives against the FIX
# gateway. Same stdin interface ("T" fires a burst) so the harness can drive
# either, but it reports its own sent/received counts and per-order round-trip
# latency rather than leaving the harness to infer completion from log lines.
add_executable(binary_load_client
        tools/BinaryLoadClientMain.cpp
)

target_include_directories(binary_load_client PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(binary_load_client PRIVATE
        pubsub_itc_fw
        pthread
)

add_dependencies(binary_load_client fix_orders_generated binary_session_generated)

install(TARGETS binary_load_client
        RUNTIME DESTINATION bin
)

# ── Unit tests ────────────────────────────────────────────────────────────────
add_executable(binary_order_gateway_tests
        tests/BinaryProtocolTest.cpp
)

target_include_directories(binary_order_gateway_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(binary_order_gateway_tests PRIVATE
        pubsub_itc_fw
        tsl::robin_map
        GTest::gtest_main
)

add_dependencies(binary_order_gateway_tests fix_orders_generated binary_session_generated)

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(binary_order_gateway_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(binary_order_gateway_tests)
endif()

install(TARGETS binary_order_gateway_tests
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

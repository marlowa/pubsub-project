add_executable(matching_engine
        MatchingEngineConfigurationLoader.cpp
        MatchingEngineThread.cpp
        MatchingEngine.cpp
        OrderBook.cpp
)

target_include_directories(matching_engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(matching_engine PRIVATE
        pubsub_itc_fw
)

add_dependencies(matching_engine fix_orders_generated matching_engine_replication_generated leader_follower_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(matching_engine PRIVATE
        MatchingEngineConfiguration.hpp
        MatchingEngineConfigurationLoader.hpp
        MatchingEngineThread.hpp
        MatchingEngine.hpp
        OrderBook.hpp
        OrderEntry.hpp
)

install(TARGETS matching_engine
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/matching_engine_primary.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/matching_engine_secondary.toml
        DESTINATION etc/matching_engine
)

# ── Unit tests ────────────────────────────────────────────────────────────────
# The order book key decides which orders are the same order, and got that wrong once
# when a second gateway arrived. It is a plain value type, so it is worth testing on its
# own rather than only through the running engine.
add_executable(matching_engine_tests
        tests/OrderKeyTest.cpp
        tests/CancelClOrdIdTest.cpp
        tests/OrderBookTest.cpp
        OrderBook.cpp
)

target_include_directories(matching_engine_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(matching_engine_tests PRIVATE
        pubsub_itc_fw
        GTest::gtest_main
)

add_dependencies(matching_engine_tests fix_orders_generated)

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(matching_engine_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(matching_engine_tests)
endif()

install(TARGETS matching_engine_tests
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# ── Performance harness ───────────────────────────────────────────────────────
# What the open-order region costs the engine, against the book as it was.
add_subdirectory(performance)

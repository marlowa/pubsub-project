add_executable(matching_engine
        Ff1Cipher.cpp
        MatchingEngineConfigurationLoader.cpp
        MatchingEngineStateMachine.cpp
        MatchingEngineThread.cpp
        MatchingEngine.cpp
        OrderBook.cpp
        ReportIdentifiers.cpp
)

target_include_directories(matching_engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(matching_engine PRIVATE
        pubsub_itc_fw
        OpenSSL::Crypto
)

add_dependencies(matching_engine fix_orders_generated matching_engine_replication_generated matching_engine_test_events_generated leader_follower_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(matching_engine PRIVATE
        Ff1Cipher.hpp
        MatchingEngineConfiguration.hpp
        MatchingEngineConfigurationLoader.hpp
        MatchingEngineStateMachine.hpp
        MatchingEngineThread.hpp
        MatchingEngine.hpp
        OrderBook.hpp
        OrderEntry.hpp
        ReportIdentifiers.hpp
)

install(TARGETS matching_engine
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/matching_engine_primary.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/matching_engine_secondary.toml
        DESTINATION etc/matching_engine
)

# ── wal_replay_check ──────────────────────────────────────────────────────────
# Whether a fresh state machine, given a sequencer's WAL, produces the reports the WAL holds. See
# docs/availability/deterministic_matching_engine.md, section 7.2.
add_executable(wal_replay_check
        tools/WalReplayCheckMain.cpp
        Ff1Cipher.cpp
        MatchingEngineStateMachine.cpp
        OrderBook.cpp
        ReportIdentifiers.cpp
)

target_include_directories(wal_replay_check PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(wal_replay_check PRIVATE
        pubsub_itc_fw
        OpenSSL::Crypto
)

add_dependencies(wal_replay_check fix_orders_generated leader_follower_generated matching_engine_test_events_generated)

install(TARGETS wal_replay_check
        RUNTIME DESTINATION bin
)

# ── identifier_to_position ────────────────────────────────────────────────────
# The position in the WAL an OrderID or ExecID was made from, for support staff. See
# docs/availability/deterministic_matching_engine.md, section 3.4.
add_executable(identifier_to_position
        tools/IdentifierToPositionMain.cpp
        Ff1Cipher.cpp
        ReportIdentifiers.cpp
)

target_include_directories(identifier_to_position PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
)

target_link_libraries(identifier_to_position PRIVATE
        pubsub_itc_fw
        OpenSSL::Crypto
)

install(TARGETS identifier_to_position
        RUNTIME DESTINATION bin
)

# ── Unit tests ────────────────────────────────────────────────────────────────
# The order book key decides which orders are the same order, and got that wrong once
# when a second gateway arrived. It is a plain value type, so it is worth testing on its
# own rather than only through the running engine.
add_executable(matching_engine_tests
        tests/OrderKeyTest.cpp
        tests/CancelClOrdIdTest.cpp
        tests/Ff1CipherTest.cpp
        tests/OrderBookTest.cpp
        tests/MatchingEngineStateMachineTest.cpp
        tests/ReportIdentifiersTest.cpp
        Ff1Cipher.cpp
        MatchingEngineStateMachine.cpp
        OrderBook.cpp
        ReportIdentifiers.cpp
)

target_include_directories(matching_engine_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(matching_engine_tests PRIVATE
        pubsub_itc_fw
        OpenSSL::Crypto
        GTest::gtest_main
)

add_dependencies(matching_engine_tests fix_orders_generated matching_engine_test_events_generated)

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

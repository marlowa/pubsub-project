add_executable(sequencer
        SequencerConfigurationLoader.cpp
        SequencerThread.cpp
        Sequencer.cpp
)

target_include_directories(sequencer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(sequencer PRIVATE
        pubsub_itc_fw
)

add_dependencies(sequencer fix_orders_generated leader_follower_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(sequencer PRIVATE
        SequencerConfiguration.hpp
        SequencerConfigurationLoader.hpp
        SequencerThread.hpp
        Sequencer.hpp
)

install(TARGETS sequencer
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/sequencer_primary.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/sequencer_secondary.toml
        DESTINATION etc/sequencer
)

# ── Test tool ─────────────────────────────────────────────────────────────────
# Hands one order or cancel straight to a sequencer, bypassing the gateways' checks, so that
# ha_test.py can test the matching engine's own refusals (BUG-0101). Not for a live venue.
add_executable(inject_order
        tools/InjectOrderMain.cpp
)

target_include_directories(inject_order PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(inject_order PRIVATE
        pubsub_itc_fw
)

add_dependencies(inject_order fix_orders_generated leader_follower_generated authentication_generated)

install(TARGETS inject_order
        RUNTIME DESTINATION bin
)

# Loaded with LD_PRELOAD into one sequencer by ha_test.py, to stop what it sends to its peer from
# arriving once a flag file appears, so that a leader can be made to die holding records its
# follower never received (BUG-0103). Not for a live venue.
add_library(block_sends_to_ports SHARED
        tools/BlockSendsToPorts.cpp
)

target_link_libraries(block_sends_to_ports PRIVATE
        ${CMAKE_DL_LIBS}
)

install(TARGETS block_sends_to_ports
        LIBRARY DESTINATION lib
)

# ── Unit tests ────────────────────────────────────────────────────────────────
# The configuration loader is the part worth testing here: it grew an array-of-tables
# parse with duplicate detection and an enabled filter, and a mistake in it produces a
# sequencer that starts and then delivers reports nowhere.
add_executable(sequencer_tests
        tests/SequencerConfigurationLoaderTest.cpp
        tests/EpochStoreTest.cpp
        tests/LeaderEpochTest.cpp
        tests/LeaseRulesTest.cpp
        tests/LeaseSimulationTest.cpp
        tests/PairLeaseAgentTest.cpp
        tests/EngineOrderRoutingTest.cpp
        tests/LeasePromiseStoreTest.cpp
        tests/KeptReportStoreTest.cpp
        tests/LogEpochTableTest.cpp
        tests/WalRecordEncodingTest.cpp
        SequencerConfigurationLoader.cpp
)

target_include_directories(sequencer_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(sequencer_tests PRIVATE
        pubsub_itc_fw
        pubsub_itc_fw_test_helpers
        GTest::gtest_main
)

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(sequencer_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(sequencer_tests)
endif()

install(TARGETS sequencer_tests
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

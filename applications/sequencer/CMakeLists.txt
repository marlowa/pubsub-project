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

# ── Unit tests ────────────────────────────────────────────────────────────────
# The configuration loader is the part worth testing here: it grew an array-of-tables
# parse with duplicate detection and an enabled filter, and a mistake in it produces a
# sequencer that starts and then delivers reports nowhere.
add_executable(sequencer_tests
        tests/SequencerConfigurationLoaderTest.cpp
        tests/EpochStoreTest.cpp
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

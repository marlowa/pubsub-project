# The arbiter program: one of the two arbiters that, with the witness, vote on which instance of each component pair leads.
# Built on the library's lease machinery and voting logic (docs/venue/arbiter.md).
add_executable(arbiter
        main.cpp
        Arbiter.cpp
        ArbiterThread.cpp
        ArbiterConfigurationLoader.cpp
)

target_include_directories(arbiter PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(arbiter PRIVATE
        pubsub_itc_fw
)

install(TARGETS arbiter
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/arbiter_primary.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/arbiter_secondary.toml
        DESTINATION etc/arbiter
)

# ── Unit tests ────────────────────────────────────────────────────────────────
add_executable(arbiter_tests
        tests/ArbiterConfigurationLoaderTest.cpp
        Arbiter.cpp
        ArbiterThread.cpp
        ArbiterConfigurationLoader.cpp
)

target_include_directories(arbiter_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(arbiter_tests PRIVATE
        pubsub_itc_fw
        GTest::gtest_main
)

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(arbiter_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(arbiter_tests)
endif()

install(TARGETS arbiter_tests
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

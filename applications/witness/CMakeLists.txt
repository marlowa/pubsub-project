# The witness program: the third voter, with the two arbiters, in deciding which arbiter is active.
# Built on the library's lease machinery and voting logic (docs/venue/witness.md).
add_executable(witness
        main.cpp
        Witness.cpp
        WitnessThread.cpp
        WitnessConfigurationLoader.cpp
)

target_include_directories(witness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(witness PRIVATE
        pubsub_itc_fw
)

install(TARGETS witness
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/witness.toml
        DESTINATION etc/witness
)

# ── Unit tests ────────────────────────────────────────────────────────────────
add_executable(witness_tests
        tests/WitnessConfigurationLoaderTest.cpp
        Witness.cpp
        WitnessThread.cpp
        WitnessConfigurationLoader.cpp
)

target_include_directories(witness_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(witness_tests PRIVATE
        pubsub_itc_fw
        GTest::gtest_main
)

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(witness_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(witness_tests)
endif()

install(TARGETS witness_tests
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

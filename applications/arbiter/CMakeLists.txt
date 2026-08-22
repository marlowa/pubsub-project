add_executable(arbiter
        ArbiterConfigurationLoader.cpp
        ArbiterThread.cpp
        Arbiter.cpp
)

target_include_directories(arbiter PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(arbiter PRIVATE
        pubsub_itc_fw
)

add_dependencies(arbiter leader_follower_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(arbiter PRIVATE
        ArbiterConfiguration.hpp
        ArbiterConfigurationLoader.hpp
        ArbiterThread.hpp
        Arbiter.hpp
        LeadershipDecision.hpp
)

install(TARGETS arbiter
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/arbiter_primary.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/arbiter_secondary.toml
        DESTINATION etc/arbiter
)

add_executable(arbiter_tests
        tests/LeadershipDecisionTest.cpp
)

target_include_directories(arbiter_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(arbiter_tests PRIVATE
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

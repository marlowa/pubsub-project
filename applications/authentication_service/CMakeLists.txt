add_executable(authentication_service
        AuthenticationServiceConfigurationLoader.cpp
        AuthenticationThread.cpp
        AuthenticationService.cpp
        ScramExchanges.cpp
)

# fix_common for ThrottleLimits.hpp alone: the range of a throttle limit is checked here, in the
# admin service, in the database and in the gateways, and this header is where the number is kept.
target_include_directories(authentication_service PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(authentication_service PRIVATE
        pubsub_itc_fw
        scram_crypto
)

add_dependencies(authentication_service authentication_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(authentication_service PRIVATE
        AuthenticationServiceConfiguration.hpp
        AuthenticationServiceConfigurationLoader.hpp
        AuthenticationThread.hpp
        AuthenticationService.hpp
        ScramExchanges.hpp
)

install(TARGETS authentication_service
        RUNTIME DESTINATION bin
)

# The service's decisions, tested without a reactor: the SCRAM exchange and the configuration it reads.
add_executable(authentication_service_tests
        tests/AuthenticationServiceConfigurationLoaderTest.cpp
        tests/ScramExchangesTest.cpp
        AuthenticationServiceConfigurationLoader.cpp
        ScramExchanges.cpp
)

target_include_directories(authentication_service_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../fix_common
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(authentication_service_tests PRIVATE
        pubsub_itc_fw
        pubsub_itc_fw_test_helpers
        scram_crypto
        GTest::gtest_main
)

add_dependencies(authentication_service_tests authentication_generated)

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(authentication_service_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(authentication_service_tests)
endif()

install(TARGETS authentication_service_tests
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/authentication_service_a.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/authentication_service_b.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/credentials.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/admin.crt
        ${CMAKE_CURRENT_SOURCE_DIR}/admin.key
        DESTINATION etc/authentication_service
)

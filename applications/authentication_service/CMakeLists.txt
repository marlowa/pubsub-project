add_executable(authentication_service
        AuthenticationServiceConfigurationLoader.cpp
        AuthenticationThread.cpp
        AuthenticationService.cpp
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
)

install(TARGETS authentication_service
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/authentication_service_a.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/authentication_service_b.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/credentials.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/admin.crt
        ${CMAKE_CURRENT_SOURCE_DIR}/admin.key
        DESTINATION etc/authentication_service
)

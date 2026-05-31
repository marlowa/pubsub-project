add_executable(authentication_service
        AuthenticationServiceConfigurationLoader.cpp
        AuthenticationThread.cpp
        AuthenticationService.cpp
)

target_include_directories(authentication_service PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
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
        ${CMAKE_CURRENT_SOURCE_DIR}/authentication_service.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/authentication_service_secondary.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/credentials.toml
        DESTINATION etc/authentication_service
)

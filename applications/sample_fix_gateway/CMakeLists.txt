add_executable(sample_fix_gateway
        FixParser.cpp
        FixSerialiser.cpp
        FixGatewayConfigurationLoader.cpp
        FixGatewayThread.cpp
        SampleFixGateway.cpp
)

target_include_directories(sample_fix_gateway PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sample_fix_gateway PRIVATE
        pubsub_itc_fw
)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(sample_fix_gateway PRIVATE
        FixGatewayConfiguration.hpp
        FixGatewayConfigurationLoader.hpp
        FixGatewayThread.hpp
        FixMessage.hpp
        FixParser.hpp
        FixSerialiser.hpp
        FixSession.hpp
        SampleFixGateway.hpp
)

install(TARGETS sample_fix_gateway
        RUNTIME DESTINATION bin
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/sample_fix_gateway.toml
        DESTINATION etc/sample_fix_gateway
)

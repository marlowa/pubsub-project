add_executable(sample_fix_gateway_seq
        FixErEncoder.cpp
        FixGatewaySeqConfigurationLoader.cpp
        FixGatewaySeqThread.cpp
        FixParser.cpp
        FixSerialiser.cpp
        SampleFixGatewaySeq.cpp
)

target_include_directories(sample_fix_gateway_seq PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(sample_fix_gateway_seq PRIVATE
        pubsub_itc_fw
        scram_crypto
)

add_dependencies(sample_fix_gateway_seq fix_equity_orders_generated authentication_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(sample_fix_gateway_seq PRIVATE
        FixErEncoder.hpp
        FixGatewaySeqConfiguration.hpp
        FixGatewaySeqConfigurationLoader.hpp
        FixGatewaySeqThread.hpp
        FixMessage.hpp
        FixParser.hpp
        FixSerialiser.hpp
        FixSession.hpp
        SampleFixGatewaySeq.hpp
)

install(TARGETS sample_fix_gateway_seq
        RUNTIME DESTINATION bin
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/sample_fix_gateway_seq.toml
        DESTINATION etc/sample_fix_gateway_seq
)

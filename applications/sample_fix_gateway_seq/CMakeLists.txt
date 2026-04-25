add_executable(sample_fix_gateway_seq
        FixGatewaySeqConfigurationLoader.cpp
        FixGatewaySeqThread.cpp
        SampleFixGatewaySeq.cpp
)

target_include_directories(sample_fix_gateway_seq PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_link_libraries(sample_fix_gateway_seq PRIVATE
        pubsub_itc_fw
)

add_dependencies(sample_fix_gateway_seq fix_equity_orders_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(sample_fix_gateway_seq PRIVATE
        FixGatewaySeqConfiguration.hpp
        FixGatewaySeqConfigurationLoader.hpp
        FixGatewaySeqThread.hpp
        SampleFixGatewaySeq.hpp
)

install(TARGETS sample_fix_gateway_seq
        RUNTIME DESTINATION bin
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/sample_fix_gateway_seq.toml
        DESTINATION etc/sample_fix_gateway_seq
)

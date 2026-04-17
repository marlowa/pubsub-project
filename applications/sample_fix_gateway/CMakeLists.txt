add_executable(sample_fix_gateway
        FixParser.cpp
        FixSerialiser.cpp
        FixGatewayThread.cpp
        SampleFixGateway.cpp
)

target_include_directories(sample_fix_gateway PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sample_fix_gateway PRIVATE
        pubsub_itc_fw
)

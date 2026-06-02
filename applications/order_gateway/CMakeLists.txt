add_executable(order_gateway
        FixErEncoder.cpp
        OrderGatewayConfigurationLoader.cpp
        OrderGatewayThread.cpp
        FixParser.cpp
        FixSerialiser.cpp
        OrderGateway.cpp
)

target_include_directories(order_gateway PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(order_gateway PRIVATE
        pubsub_itc_fw
        scram_crypto
)

add_dependencies(order_gateway fix_equity_orders_generated authentication_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(order_gateway PRIVATE
        FixErEncoder.hpp
        OrderGatewayConfiguration.hpp
        OrderGatewayConfigurationLoader.hpp
        OrderGatewayThread.hpp
        FixMessage.hpp
        FixParser.hpp
        FixSerialiser.hpp
        FixSession.hpp
        OrderGateway.hpp
)

install(TARGETS order_gateway
        RUNTIME DESTINATION bin
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/order_gateway.toml
        DESTINATION etc/order_gateway
)

add_executable(matching_engine
        MatchingEngineConfigurationLoader.cpp
        MatchingEngineThread.cpp
        MatchingEngine.cpp
)

target_include_directories(matching_engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_link_libraries(matching_engine PRIVATE
        pubsub_itc_fw
)

add_dependencies(matching_engine fix_equity_orders_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(matching_engine PRIVATE
        MatchingEngineConfiguration.hpp
        MatchingEngineConfigurationLoader.hpp
        MatchingEngineThread.hpp
        MatchingEngine.hpp
)

install(TARGETS matching_engine
        RUNTIME DESTINATION bin
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/matching_engine.toml
        DESTINATION etc/matching_engine
)

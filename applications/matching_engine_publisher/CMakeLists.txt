add_executable(matching_engine_publisher
        MatchingEnginePublisherConfigurationLoader.cpp
        MatchingEnginePublisherThread.cpp
        MatchingEnginePublisher.cpp
)

target_include_directories(matching_engine_publisher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(matching_engine_publisher PRIVATE
        pubsub_itc_fw
)

add_dependencies(matching_engine_publisher
        leader_follower_generated
        topics_generated
)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(matching_engine_publisher PRIVATE
        MatchingEnginePublisherConfiguration.hpp
        MatchingEnginePublisherConfigurationLoader.hpp
        MatchingEnginePublisherThread.hpp
        MatchingEnginePublisher.hpp
)

install(TARGETS matching_engine_publisher
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/matching_engine_publisher.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/matching_engine_publisher_secondary.toml
        DESTINATION etc/matching_engine_publisher
)

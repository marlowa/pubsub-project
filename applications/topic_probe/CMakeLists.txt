add_executable(topic_probe
        TopicProbe.cpp
)

target_include_directories(topic_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(topic_probe PRIVATE
        pubsub_itc_fw
        argparse::argparse
)

# topic_probe includes:
#   - the topic PDU views it dumps (topics.hpp, from topics_generated)
#   - the topic registry: Topic enum, all_topics, topic_from_name (topics_registry.hpp,
#     from topics_registry_generated)
#   - the fix_orders message views/decoders it dumps (fix_orders.hpp, from fix_orders_generated)
add_dependencies(topic_probe
        topics_generated
        topics_registry_generated
        fix_orders_generated
)

install(TARGETS topic_probe
        RUNTIME DESTINATION bin
)

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

# topic_probe includes both the topic registry (topics_generated) and the
# fix_orders message views/decoders it dumps (fix_orders_generated).
add_dependencies(topic_probe
        topics_generated
        fix_orders_generated
)

install(TARGETS topic_probe
        RUNTIME DESTINATION bin
)

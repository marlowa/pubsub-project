# The arbiter program: a short main that reads its configuration and runs the library's arbiter,
# part of the voting service in libraries/pubsub_itc_fw (docs/venue/arbiter.md).
add_executable(arbiter
        main.cpp
)

target_link_libraries(arbiter PRIVATE
        pubsub_itc_fw
)

install(TARGETS arbiter
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/arbiter_primary.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/arbiter_secondary.toml
        DESTINATION etc/arbiter
)

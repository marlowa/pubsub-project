# The witness program: a short main that reads its configuration and runs the library's witness,
# part of the voting service in libraries/pubsub_itc_fw (docs/venue/witness.md).
add_executable(witness
        main.cpp
)

target_link_libraries(witness PRIVATE
        pubsub_itc_fw
)

install(TARGETS witness
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/witness.toml
        DESTINATION etc/witness
)

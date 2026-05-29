add_executable(witness
        WitnessConfigurationLoader.cpp
        WitnessThread.cpp
        Witness.cpp
)

target_include_directories(witness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FIX_DSL_INCLUDE_DIR}
)

target_link_libraries(witness PRIVATE
        pubsub_itc_fw
)

add_dependencies(witness leader_follower_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(witness PRIVATE
        WitnessConfiguration.hpp
        WitnessConfigurationLoader.hpp
        WitnessThread.hpp
        Witness.hpp
)

install(TARGETS witness
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/witness.toml
        DESTINATION etc/witness
)

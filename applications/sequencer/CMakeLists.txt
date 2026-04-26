add_executable(sequencer
        SequencerConfigurationLoader.cpp
        SequencerThread.cpp
        Sequencer.cpp
)

target_include_directories(sequencer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_link_libraries(sequencer PRIVATE
        pubsub_itc_fw
)

add_dependencies(sequencer fix_equity_orders_generated)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(sequencer PRIVATE
        SequencerConfiguration.hpp
        SequencerConfigurationLoader.hpp
        SequencerThread.hpp
        Sequencer.hpp
)

install(TARGETS sequencer
        RUNTIME DESTINATION bin
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/sequencer.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/sequencer_secondary.toml
        DESTINATION etc/sequencer
)

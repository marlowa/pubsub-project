add_executable(arbiter
        ArbiterConfigurationLoader.cpp
        ArbiterThread.cpp
        Arbiter.cpp
)

target_include_directories(arbiter PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(arbiter PRIVATE
        pubsub_itc_fw
)

# Header files listed explicitly so CLion indexes them correctly.
target_sources(arbiter PRIVATE
        ArbiterConfiguration.hpp
        ArbiterConfigurationLoader.hpp
        ArbiterThread.hpp
        Arbiter.hpp
)

install(TARGETS arbiter
        RUNTIME DESTINATION bin
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/arbiter.toml
        DESTINATION etc/arbiter
)

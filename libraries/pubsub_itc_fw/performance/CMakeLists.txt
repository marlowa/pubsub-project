# CMakeLists.txt for pubsub_itc_fw performance harnesses

if(ENABLE_COVERAGE OR ENABLE_ASAN OR ENABLE_TSAN)
    message(STATUS "Performance harnesses disabled (coverage/sanitizer build)")
    return()
endif()

message(STATUS "Building performance harnesses for pubsub_itc_fw")

add_executable(fixed_pool_bench
    src/FixedPoolBench.cpp
)

target_link_libraries(fixed_pool_bench PRIVATE
    pubsub_itc_fw
    pthread
)

target_include_directories(fixed_pool_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_compile_options(fixed_pool_bench PRIVATE
    -O3
    -g
    -fno-omit-frame-pointer
)

if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "11.0")
    target_compile_options(fixed_pool_bench PRIVATE -mcx16 -march=x86-64-v2)
else()
    target_compile_options(fixed_pool_bench PRIVATE -mcx16 -msse4.2 -mpopcnt)
endif()

message(STATUS "  Added performance harness: fixed_pool_bench")

# -----------------------------------------------------------------------------
# topic_pubsub_bench: end-to-end topic pub/sub throughput + latency harness
# -----------------------------------------------------------------------------
add_executable(topic_pubsub_bench
    src/TopicPubSubBench.cpp
)

add_dependencies(topic_pubsub_bench
    topics_generated
)

target_link_libraries(topic_pubsub_bench PRIVATE
    pubsub_itc_fw
    pthread
)

target_include_directories(topic_pubsub_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${FIX_DSL_INCLUDE_DIR}
)

target_compile_options(topic_pubsub_bench PRIVATE
    -O3
    -g
    -fno-omit-frame-pointer
)

message(STATUS "  Added performance harness: topic_pubsub_bench")

# Install performance harnesses -> bin
install(TARGETS fixed_pool_bench topic_pubsub_bench
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

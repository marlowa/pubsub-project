# CMakeLists.txt for pubsub_itc_fw performance harnesses

# Disable performance builds when sanitizers or coverage are enabled
if(ENABLE_COVERAGE OR ENABLE_ASAN OR ENABLE_TSAN)
    message(STATUS "Performance harnesses disabled (coverage/sanitizer build)")
    return()
endif()

message(STATUS "Building performance harnesses for pubsub_itc_fw")

# ---------------------------------------------------------------------------
# fixed_pool_bench
# ---------------------------------------------------------------------------

add_executable(fixed_pool_bench
    src/FixedPoolBench.cpp
)

target_link_libraries(fixed_pool_bench PRIVATE
    pubsub_itc_fw
    pthread
)

target_include_directories(fixed_pool_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_BINARY_DIR}/../../..   # protobuf generated headers
)

# Profiling-friendly flags: optimized, debuggable, frame pointers
target_compile_options(fixed_pool_bench PRIVATE
    -O3
    -g
    -fno-omit-frame-pointer
    -mcx16
    -march=x86-64-v2
)

message(STATUS "  Added performance harness: fixed_pool_bench")

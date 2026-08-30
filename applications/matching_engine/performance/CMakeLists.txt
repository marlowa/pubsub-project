# CMakeLists.txt for matching_engine performance harnesses

if(ENABLE_COVERAGE OR ENABLE_ASAN OR ENABLE_TSAN)
    message(STATUS "Performance harnesses disabled (coverage/sanitizer build)")
    return()
endif()

# What the open-order region costs the engine's thread, against the book as it was.
add_executable(order_book_bench
    src/OrderBookBench.cpp
    ../OrderBook.cpp
)

add_dependencies(order_book_bench fix_orders_generated)

target_link_libraries(order_book_bench PRIVATE
    pubsub_itc_fw
)

target_include_directories(order_book_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../fix_common
    ${FIX_DSL_INCLUDE_DIR}
)

# No -O level here: the harness inherits the project's -O2 so that it measures the code that
# actually ships. See "Performance work" in coding-rules-for-ai-chatbots.txt.
target_compile_options(order_book_bench PRIVATE
    -g
    -fno-omit-frame-pointer
)

message(STATUS "  Added performance harness: order_book_bench")

install(TARGETS order_book_bench
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

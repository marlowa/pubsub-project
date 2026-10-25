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

# What the book costs in memory and in starting time at the size a deployment configures. See
# docs/bug_list.md, BUG-0028.
add_executable(order_book_size_bench
    src/OrderBookSizeBench.cpp
    ../OrderBook.cpp
)

add_dependencies(order_book_size_bench fix_orders_generated)

target_link_libraries(order_book_size_bench PRIVATE
    pubsub_itc_fw
)

target_include_directories(order_book_size_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../fix_common
    ${FIX_DSL_INCLUDE_DIR}
)

target_compile_options(order_book_size_bench PRIVATE
    -g
    -fno-omit-frame-pointer
)

message(STATUS "  Added performance harness: order_book_size_bench")

install(TARGETS order_book_bench order_book_size_bench
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# What making one ExecID with FF1 costs the engine's thread. See
# docs/availability/deterministic_matching_engine.md, section 3.2.
add_executable(report_identifiers_bench
    src/ReportIdentifiersBench.cpp
    ../Ff1Cipher.cpp
    ../ReportIdentifiers.cpp
)

target_link_libraries(report_identifiers_bench PRIVATE
    pubsub_itc_fw
    OpenSSL::Crypto
)

target_include_directories(report_identifiers_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../fix_common
)

target_compile_options(report_identifiers_bench PRIVATE
    -g
    -fno-omit-frame-pointer
)

message(STATUS "  Added performance harness: report_identifiers_bench")

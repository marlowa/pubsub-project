# CMakeLists.txt for pubsub_itc_fw shared library

# ------------------------------------------------------------
# DSL generation
# ------------------------------------------------------------

# Find all DSL files anywhere under libraries/pubsub_itc_fw/
file(GLOB_RECURSE DSL_SCHEMAS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.dsl
)

# Output directory for generated DSL headers
set(DSL_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/dsl)
file(MAKE_DIRECTORY ${DSL_OUTPUT_DIR})

set(GENERATED_DSL_HEADERS "")

foreach(DSL_FILE ${DSL_SCHEMAS})
    get_filename_component(DSL_NAME ${DSL_FILE} NAME_WE)
    set(OUT_HPP ${DSL_OUTPUT_DIR}/${DSL_NAME}.hpp)

    add_custom_command(
        OUTPUT ${OUT_HPP}
        COMMAND ${Python3_EXECUTABLE}
           ${CMAKE_CURRENT_SOURCE_DIR}/../../python/tools/generate_cpp_from_dsl.py
            ${DSL_FILE}
            ${OUT_HPP}
            --namespace pubsub_itc_fw
        DEPENDS ${DSL_FILE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../../python/tools/generate_cpp_from_dsl.py
                ${CMAKE_CURRENT_SOURCE_DIR}/../../python/dsl/generator_cpp.py
        COMMENT "Generating DSL header ${OUT_HPP}"
    )

    list(APPEND GENERATED_DSL_HEADERS ${OUT_HPP})
endforeach()

add_custom_target(pubsub_itc_fw_dsl
    DEPENDS ${GENERATED_DSL_HEADERS}
)

# ------------------------------------------------------------
# Library sources
# ------------------------------------------------------------
set(PUBSUB_ITC_FW_SRC
    src/ApplicationThread.cpp
    src/InetAddress.cpp
    src/EmptySlabQueue.cpp
    src/EventMessage.cpp
    src/ExpandableSlabAllocator.cpp
    src/MillisecondClock.cpp
    src/InboundConnection.cpp
    src/OutboundConnection.cpp
    src/PduFramer.cpp
    src/PduParser.cpp
    src/QuillLogger.cpp
    src/Reactor.cpp
    src/SlabAllocator.cpp
    src/StringUtils.cpp
    src/TcpAcceptor.cpp
    src/TcpConnector.cpp
    src/TcpSocket.cpp
    src/TimerHandler.cpp
    src/TomlConfiguration.cpp
    src/MirroredBuffer.cpp
    src/PduProtocolHandler.cpp
    src/InboundConnectionManager.cpp
    src/OutboundConnectionManager.cpp
    src/RawBytesProtocolHandler.cpp
)

# ------------------------------------------------------------
# Build the shared library
# ------------------------------------------------------------
add_library(pubsub_itc_fw SHARED
    ${PUBSUB_ITC_FW_SRC}
    ${PUBSUB_ITC_FW_PROTO_SRCS}
    ${GENERATED_DSL_HEADERS}   # optional but IDE-friendly
)

add_dependencies(pubsub_itc_fw
    pubsub_itc_fw_dsl
)

# ------------------------------------------------------------
# Include paths
# ------------------------------------------------------------
target_include_directories(pubsub_itc_fw PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/dsl>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(pubsub_itc_fw SYSTEM PRIVATE
        ${THIRDPARTY_DIR}/installed/tomlplusplus/${TOMLPLUSPLUS_VERSION}/include
)

# ------------------------------------------------------------
# Link dependencies
# ------------------------------------------------------------
target_link_libraries(pubsub_itc_fw PUBLIC
    Boost::boost
    fmt::fmt
    quill::quill
    argparse::argparse
    atomic
)

# ------------------------------------------------------------
# Coverage instantiation TU
# ------------------------------------------------------------
if(ENABLE_COVERAGE)
    target_sources(pubsub_itc_fw PRIVATE src/pubsub_itc_fw_coverage.cpp)

    set_source_files_properties(
        src/pubsub_itc_fw_coverage.cpp
        PROPERTIES COMPILE_FLAGS "-O0 -fno-inline -fno-inline-small-functions -fno-default-inline"
    )
endif()

# ------------------------------------------------------------
# Performance benchmark executable
# ------------------------------------------------------------

add_executable(pubsub_itc_fw_bench
    performance/DslBenchMain.cpp
)

# Ensure DSL is generated before building the bench
add_dependencies(pubsub_itc_fw_bench
    pubsub_itc_fw_dsl
)

target_link_libraries(pubsub_itc_fw_bench
    PRIVATE pubsub_itc_fw
)

target_include_directories(pubsub_itc_fw_bench PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/dsl
)

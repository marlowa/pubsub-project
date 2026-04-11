# CMakeLists.txt for pubsub_itc_fw shared library

# ------------------------------------------------------------
# Ensure protobuf output directory exists
# ------------------------------------------------------------
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw)

# ------------------------------------------------------------
# Protobuf generation
# ------------------------------------------------------------
add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/message_types.pb.cc
        ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/message_types.pb.h
        ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/system_messages.pb.cc
        ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/system_messages.pb.h
    COMMAND protobuf::protoc
        --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/pubsub_itc_fw/message_types.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/include/pubsub_itc_fw/system_messages.proto
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/pubsub_itc_fw/message_types.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/include/pubsub_itc_fw/system_messages.proto
    COMMENT "Generating protobuf sources for pubsub_itc_fw"
)

add_custom_target(pubsub_itc_fw_protos
    DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/message_types.pb.cc
        ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/system_messages.pb.cc
)

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
    src/AcceptorHandler.cpp
    src/ApplicationThread.cpp
    src/InetAddress.cpp
    src/EmptySlabQueue.cpp
    src/EventMessage.cpp
    src/ExpandableSlabAllocator.cpp
    src/Message.cpp
    src/EventDistributor.cpp
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
    src/MirroredBuffer.cpp
)

set(PUBSUB_ITC_FW_PROTO_SRCS
    ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/message_types.pb.cc
    ${CMAKE_CURRENT_BINARY_DIR}/pubsub_itc_fw/system_messages.pb.cc
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
    pubsub_itc_fw_protos
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

# ------------------------------------------------------------
# Link dependencies
# ------------------------------------------------------------
target_link_libraries(pubsub_itc_fw PUBLIC
    protobuf::libprotobuf
    Boost::headers
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

# Ensure DSL + protobuf are generated before building the bench
add_dependencies(pubsub_itc_fw_bench
    pubsub_itc_fw_dsl
    pubsub_itc_fw_protos
)

target_link_libraries(pubsub_itc_fw_bench
    PRIVATE pubsub_itc_fw
)

target_include_directories(pubsub_itc_fw_bench PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/dsl
)

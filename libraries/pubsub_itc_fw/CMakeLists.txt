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
        --cpp_out=${CMAKE_CURRENT_BINARY_DIR}   # protoc auto-creates pubsub_itc_fw/
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
# Library sources
# ------------------------------------------------------------
set(PUBSUB_ITC_FW_SRC
    src/ApplicationThread.cpp
    src/InetAddress.cpp
    src/EventMessage.cpp
    src/Message.cpp
    src/EventDistributor.cpp
    src/MillisecondClock.cpp
    src/QuillLogger.cpp
    src/Reactor.cpp
    src/StringUtils.cpp
    src/TcpAcceptor.cpp
    src/TcpConnector.cpp
    src/TcpSocket.cpp
    src/TimerHandler.cpp
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
)

add_dependencies(pubsub_itc_fw pubsub_itc_fw_protos)

# ------------------------------------------------------------
# Include paths
# ------------------------------------------------------------
target_include_directories(pubsub_itc_fw PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
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

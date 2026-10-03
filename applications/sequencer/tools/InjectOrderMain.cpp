// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// A test tool that hands one order or one cancel straight to a sequencer, as a gateway does,
// without passing through any gateway's checks.
//
// It exists to test the matching engine's own refusals. Both gateways refuse a command the
// venue cannot accept, so a test that went through a gateway could never show what the engine
// does with one: whether it refuses an over-long ClOrdID (BUG-0101), and whether it refuses it
// again when it replays the record while catching up after a restart. The command is sent in a
// WalRecord envelope, exactly as a gateway sends it, under a session that no gateway holds, so
// the engine's report reaches the sequencer and goes no further.
//
// It is not for use against a venue members are trading on: it bypasses every check the
// gateways make.
//
//   inject_order --port 11001 --cl-ord-id ORDER-1
//   inject_order --port 11001 --cl-ord-id ORDER --count 3      (ORDER-1, ORDER-2 and ORDER-3)
//   inject_order --port 11001 --cl-ord-id CANCEL-1 --cancel ORDER-1
//
// The line saying what was sent is printed, and flushed, as soon as the commands are written, before
// the tool waits for the sequencer to read them. A test that must act within milliseconds of the
// commands arriving, such as stopping the sequencer, reads that line rather than waiting for the
// tool to exit.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <pubsub_itc_fw/PduHeader.hpp>

// authentication.hpp first: it is the generated header that defines BytesView, which the others use.
#include <authentication.hpp>
#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include "GatewayIds.hpp"

namespace {

struct Options {
    std::string host{"127.0.0.1"};
    uint16_t port{11001};
    std::string comp_id{"INJECT-TEST"};
    std::string cl_ord_id;
    std::string cancel_orig_cl_ord_id;
    std::string symbol{"AAPL"};
    int count{1};
};

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const bool has_value = index + 1 < argc;
        if (argument == "--host" && has_value) {
            options.host = argv[++index];
        } else if (argument == "--port" && has_value) {
            options.port = static_cast<uint16_t>(std::stoi(argv[++index]));
        } else if (argument == "--comp-id" && has_value) {
            options.comp_id = argv[++index];
        } else if (argument == "--cl-ord-id" && has_value) {
            options.cl_ord_id = argv[++index];
        } else if (argument == "--cancel" && has_value) {
            options.cancel_orig_cl_ord_id = argv[++index];
        } else if (argument == "--symbol" && has_value) {
            options.symbol = argv[++index];
        } else if (argument == "--count" && has_value) {
            options.count = std::stoi(argv[++index]);
        } else {
            fmt::print("usage: {} [--host H] [--port P] [--comp-id ID] --cl-ord-id ID [--cancel ORIG-CL-ORD-ID] [--symbol SYM] [--count N]\n", argv[0]);
            fmt::print("\n  Sends one NewOrderSingle, or with --cancel one OrderCancelRequest, straight to the sequencer's\n");
            fmt::print("  order listener, bypassing every gateway check. For testing the matching engine only.\n");
            return false;
        }
    }
    if (options.cl_ord_id.empty()) {
        fmt::print("--cl-ord-id is required\n");
        return false;
    }
    if (options.count < 1 || (options.count > 1 && !options.cancel_orig_cl_ord_id.empty())) {
        fmt::print("--count must be at least 1, and more than 1 only for new orders\n");
        return false;
    }
    return true;
}

int64_t now_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/** @brief Encodes a message into a right-sized buffer. */
template <typename MessageType> std::vector<uint8_t> encode_message(const MessageType& message) {
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    // The measuring pass reports a zero-size buffer as too small but sets bytes_needed, which is
    // all it is for.
    static_cast<void>(encode(message, nullptr, 0, bytes_written, bytes_needed));
    std::vector<uint8_t> buffer(bytes_needed);
    if (!encode(message, buffer.data(), buffer.size(), bytes_written, bytes_needed)) {
        buffer.clear();
    }
    return buffer;
}

bool write_fully(int socket_fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const ssize_t written = ::send(socket_fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (written <= 0) {
            return false;
        }
        sent += static_cast<size_t>(written);
    }
    return true;
}

/** @brief Frames an encoded payload behind a PduHeader and writes it to the socket. */
bool send_frame(int socket_fd, int16_t pdu_id, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame(sizeof(pubsub_itc_fw::PduHeader) + payload.size());
    auto* header = reinterpret_cast<pubsub_itc_fw::PduHeader*>(frame.data());
    header->byte_count = htonl(static_cast<uint32_t>(payload.size()));
    header->pdu_id = static_cast<int16_t>(htons(static_cast<uint16_t>(pdu_id)));
    header->version = 1;
    header->filler_a = 0;
    header->seq_no = 0;
    header->canary = htonl(pubsub_itc_fw::pdu_canary_value);
    header->filler_b = 0;
    std::copy(payload.begin(), payload.end(), frame.begin() + sizeof(pubsub_itc_fw::PduHeader));
    return write_fully(socket_fd, frame.data(), frame.size());
}

int connect_to(const Options& options) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        std::perror("socket");
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.host.c_str(), &address.sin_addr) != 1 ||
        ::connect(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::perror("connect");
        ::close(socket_fd);
        return -1;
    }
    const int no_delay = 1;
    ::setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    return socket_fd;
}

/** @brief The framed envelope for one order or cancel, as a gateway sends it; empty if it cannot be encoded. */
std::vector<uint8_t> encode_command(const Options& options, const std::string& cl_ord_id) {
    std::vector<uint8_t> payload;
    int16_t inner_pdu_id = 0;
    if (options.cancel_orig_cl_ord_id.empty()) {
        pubsub_itc_fw_app::NewOrderSingle order{};
        order.cl_ord_id = cl_ord_id;
        order.side = pubsub_itc_fw_app::Side::Buy;
        order.symbol = options.symbol;
        order.ord_type = pubsub_itc_fw_app::OrdType::Limit;
        order.transact_time = now_nanoseconds();
        order.order_qty = "100";
        order.has_price = true;
        order.price = "100.00";
        payload = encode_message(order);
        inner_pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle);
    } else {
        pubsub_itc_fw_app::OrderCancelRequest cancel{};
        cancel.cl_ord_id = cl_ord_id;
        cancel.orig_cl_ord_id = options.cancel_orig_cl_ord_id;
        cancel.side = pubsub_itc_fw_app::Side::Buy;
        cancel.symbol = options.symbol;
        cancel.transact_time = now_nanoseconds();
        payload = encode_message(cancel);
        inner_pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest);
    }
    if (payload.empty()) {
        return payload;
    }

    // The envelope a gateway would send. The session belongs to no gateway, so the engine's report
    // is routed nowhere once it reaches the sequencer, which is all a test of the engine needs.
    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.pdu_id = inner_pdu_id;
    envelope.payload.data = payload.data();
    envelope.payload.size = payload.size();
    envelope.has_gateway_session_conn_id = true;
    envelope.gateway_session_conn_id = 999999;
    envelope.has_origin_gateway_id = true;
    envelope.origin_gateway_id = gateway_ids::binary_order_gateway;
    envelope.has_sender_comp_id = true;
    envelope.sender_comp_id = options.comp_id;
    return encode_message(envelope);
}

} // namespaces

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }

    std::vector<std::string> cl_ord_ids;
    for (int index = 1; index <= options.count; ++index) {
        cl_ord_ids.push_back(options.count == 1 ? options.cl_ord_id : fmt::format("{}-{}", options.cl_ord_id, index));
    }
    std::vector<std::vector<uint8_t>> frames;
    for (const std::string& cl_ord_id : cl_ord_ids) {
        frames.push_back(encode_command(options, cl_ord_id));
        if (frames.back().empty()) {
            fmt::print("could not encode the command for ClOrdID={}\n", cl_ord_id);
            return 1;
        }
    }

    const int socket_fd = connect_to(options);
    if (socket_fd < 0) {
        return 1;
    }
    bool sent = true;
    for (const std::vector<uint8_t>& frame_payload : frames) {
        sent = sent && send_frame(socket_fd, pubsub_itc_fw_app::WalRecord::message_pdu_id, frame_payload);
    }
    if (sent) {
        fmt::print("sent {} {} ClOrdID={}{}\n", cl_ord_ids.size(), options.cancel_orig_cl_ord_id.empty() ? "NewOrderSingle" : "OrderCancelRequest",
                   fmt::join(cl_ord_ids, ","), options.cancel_orig_cl_ord_id.empty() ? std::string() : " OrigClOrdID=" + options.cancel_orig_cl_ord_id);
        std::fflush(stdout);
    }
    // A moment for the sequencer to read the frames before the connection closes under them.
    ::usleep(200000);
    ::close(socket_fd);
    if (!sent) {
        fmt::print("could not send the command\n");
        return 1;
    }
    return 0;
}

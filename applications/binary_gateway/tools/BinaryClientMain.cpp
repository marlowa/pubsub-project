// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// A minimal client for the binary gateway: logs on, sends NewOrderSingles, and prints
// the ExecutionReports that come back. It exists to prove the gateway end to end
// against the real pipeline, and to serve as the reference for what a binary client
// has to do -- which is very little, and that is the point.
//
// It speaks the wire protocol with plain sockets and the generated encode/decode
// functions rather than through the framework, so it also demonstrates that a client
// needs nothing from pubsub_itc_fw beyond the PDU header layout.
//
//   binary_client --host 127.0.0.1 --port 9890 --comp-id BINCLIENT --orders 5

#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>

#include <binary_session.hpp>
#include <fix_orders.hpp>

namespace {

constexpr size_t decode_arena_size = 64 * 1024;

struct Options {
    std::string host{"127.0.0.1"};
    uint16_t port{9890};
    std::string comp_id{"BINCLIENT"};
    std::string password{"stubpassword"};
    std::string target_comp_id{"BINARY-GATEWAY"};
    std::string symbol{"AAPL"};
    int order_count{1};
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
        } else if (argument == "--password" && has_value) {
            options.password = argv[++index];
        } else if (argument == "--target-comp-id" && has_value) {
            options.target_comp_id = argv[++index];
        } else if (argument == "--symbol" && has_value) {
            options.symbol = argv[++index];
        } else if (argument == "--orders" && has_value) {
            options.order_count = std::stoi(argv[++index]);
        } else {
            std::printf("usage: %s [--host H] [--port P] [--comp-id ID] [--password P]\n", argv[0]);
            std::printf("          [--target-comp-id ID] [--symbol SYM] [--orders N]\n");
            return false;
        }
    }
    return true;
}

int connect_to_gateway(const Options& options) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        std::perror("socket");
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.host.c_str(), &address.sin_addr) != 1) {
        std::printf("bad host address '%s'\n", options.host.c_str());
        ::close(socket_fd);
        return -1;
    }
    if (::connect(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::perror("connect");
        ::close(socket_fd);
        return -1;
    }

    const int no_delay = 1;
    ::setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    return socket_fd;
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

bool read_fully(int socket_fd, uint8_t* data, size_t size) {
    size_t received = 0;
    while (received < size) {
        const ssize_t got = ::recv(socket_fd, data + received, size - received, 0);
        if (got <= 0) {
            return false;
        }
        received += static_cast<size_t>(got);
    }
    return true;
}

/** @brief Frames a message behind a PduHeader and writes it to the socket. */
template <typename MessageType> bool send_pdu(int socket_fd, int16_t pdu_id, const MessageType& message) {
    // Measuring pass. Its return value is deliberately ignored: a zero-size buffer is
    // reported as too small, but bytes_needed is set regardless, which is the whole
    // point of the call. Only the second pass, into a right-sized buffer, can fail
    // meaningfully.
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    static_cast<void>(encode(message, nullptr, 0, bytes_written, bytes_needed));
    if (bytes_needed == 0) {
        return false;
    }

    std::vector<uint8_t> frame(sizeof(pubsub_itc_fw::PduHeader) + bytes_needed);
    auto* header = reinterpret_cast<pubsub_itc_fw::PduHeader*>(frame.data());
    header->byte_count = htonl(static_cast<uint32_t>(bytes_needed));
    header->pdu_id = static_cast<int16_t>(htons(static_cast<uint16_t>(pdu_id)));
    header->version = 1;
    header->filler_a = 0;
    header->seq_no = 0;
    header->canary = htonl(pubsub_itc_fw::pdu_canary_value);
    header->filler_b = 0;

    if (!encode(message, frame.data() + sizeof(pubsub_itc_fw::PduHeader), bytes_needed, bytes_written, bytes_needed)) {
        return false;
    }
    return write_fully(socket_fd, frame.data(), frame.size());
}

/** @brief Reads one framed PDU, returning its id and leaving the payload in @p payload. */
bool receive_pdu(int socket_fd, int16_t& pdu_id, int64_t& seq_no, std::vector<uint8_t>& payload) {
    pubsub_itc_fw::PduHeader header{};
    if (!read_fully(socket_fd, reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
        return false;
    }
    if (ntohl(header.canary) != pubsub_itc_fw::pdu_canary_value) {
        std::printf("bad canary in PDU header -- stream is out of sync\n");
        return false;
    }
    pdu_id = static_cast<int16_t>(ntohs(static_cast<uint16_t>(header.pdu_id)));
    seq_no = static_cast<int64_t>(be64toh(static_cast<uint64_t>(header.seq_no)));
    payload.resize(ntohl(header.byte_count));
    return payload.empty() || read_fully(socket_fd, payload.data(), payload.size());
}

int64_t now_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespaces

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }

    const int socket_fd = connect_to_gateway(options);
    if (socket_fd < 0) {
        return 1;
    }
    std::printf("connected to %s:%u\n", options.host.c_str(), options.port);

    std::vector<uint8_t> arena_buffer(decode_arena_size);
    std::vector<uint8_t> payload;
    int16_t pdu_id = 0;
    int64_t seq_no = 0;

    pubsub_itc_fw_app::Logon logon{};
    logon.comp_id = options.comp_id;
    logon.password = options.password;
    logon.target_comp_id = options.target_comp_id;
    if (!send_pdu(socket_fd, pubsub_itc_fw_app::Logon::message_pdu_id, logon)) {
        std::printf("failed to send Logon\n");
        ::close(socket_fd);
        return 1;
    }

    if (!receive_pdu(socket_fd, pdu_id, seq_no, payload) || pdu_id != pubsub_itc_fw_app::LogonAck::message_pdu_id) {
        std::printf("did not get a LogonAck (pdu_id=%d)\n", pdu_id);
        ::close(socket_fd);
        return 1;
    }
    {
        pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
        size_t bytes_consumed = 0;
        size_t arena_bytes_needed = 0;
        pubsub_itc_fw_app::LogonAckView ack{};
        if (!pubsub_itc_fw_app::decode(ack, payload.data(), payload.size(), bytes_consumed, arena, arena_bytes_needed)) {
            std::printf("failed to decode LogonAck\n");
            ::close(socket_fd);
            return 1;
        }
        if (ack.outcome != pubsub_itc_fw_app::LogonOutcome::Accepted) {
            std::printf("logon refused: %s (%s)\n", pubsub_itc_fw_app::to_string(ack.outcome).data(),
                        ack.has_text ? std::string(ack.text).c_str() : "no detail");
            ::close(socket_fd);
            return 1;
        }
    }
    std::printf("logged on as '%s'\n", options.comp_id.c_str());

    for (int order = 0; order < options.order_count; ++order) {
        const std::string cl_ord_id = options.comp_id + "-" + std::to_string(now_nanoseconds()) + "-" + std::to_string(order);

        pubsub_itc_fw_app::NewOrderSingle order_message{};
        order_message.cl_ord_id = cl_ord_id;
        order_message.side = pubsub_itc_fw_app::Side::Buy;
        order_message.symbol = options.symbol;
        order_message.ord_type = pubsub_itc_fw_app::OrdType::Limit;
        order_message.transact_time = now_nanoseconds();
        order_message.order_qty = "100";
        order_message.has_price = true;
        order_message.price = "100.00";

        if (!send_pdu(socket_fd, pubsub_itc_fw_app::NewOrderSingle::message_pdu_id, order_message)) {
            std::printf("failed to send NewOrderSingle\n");
            ::close(socket_fd);
            return 1;
        }
        std::printf("sent NewOrderSingle ClOrdID=%s\n", cl_ord_id.c_str());
    }

    for (int received = 0; received < options.order_count; ++received) {
        if (!receive_pdu(socket_fd, pdu_id, seq_no, payload)) {
            std::printf("connection closed after %d execution report(s)\n", received);
            ::close(socket_fd);
            return 1;
        }
        if (pdu_id != pubsub_itc_fw_app::ExecutionReport::message_pdu_id) {
            std::printf("unexpected PDU id %d\n", pdu_id);
            continue;
        }

        pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
        size_t bytes_consumed = 0;
        size_t arena_bytes_needed = 0;
        pubsub_itc_fw_app::ExecutionReportView report{};
        if (!pubsub_itc_fw_app::decode(report, payload.data(), payload.size(), bytes_consumed, arena, arena_bytes_needed)) {
            std::printf("failed to decode ExecutionReport\n");
            continue;
        }
        std::printf("ExecutionReport seq=%lld ClOrdID=%.*s OrdStatus=%s ExecType=%s Symbol=%.*s\n", static_cast<long long>(seq_no),
                    static_cast<int>(report.cl_ord_id.size()), report.cl_ord_id.data(), pubsub_itc_fw_app::to_string(report.ord_status).data(),
                    pubsub_itc_fw_app::to_string(report.exec_type).data(), static_cast<int>(report.symbol.size()), report.symbol.data());
    }

    ::close(socket_fd);
    std::printf("done\n");
    return 0;
}

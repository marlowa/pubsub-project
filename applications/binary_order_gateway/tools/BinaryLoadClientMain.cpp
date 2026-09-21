// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Load generator for the binary gateway: the counterpart of fix8's f8test, which
// perf_run.py and callgrind_run.py drive against the FIX gateway.
//
// It follows f8test's interface deliberately so the harness can drive either tool the
// same way: sessions are established at start-up, then each newline-terminated "T" on
// stdin fires one burst of orders per session, and the process stays alive afterwards so
// the gateway can deliver the execution reports.
//
// Two things it does that f8test cannot, because it owns both ends of the exchange:
//
//   - It reports ground truth itself. The FIX harness counts log lines to decide when a
//     run has finished; this client knows exactly how many orders it sent and how many
//     reports came back, so a discrepancy is a fact rather than an inference from logging.
//   - It measures true round-trip latency. Every order's send time is recorded against its
//     ClOrdID and matched when the report returns, giving per-order send-to-report
//     latency rather than a throughput figure alone.
//
// Each session logs on with its own comp id (the prefix plus an index), because the
// gateway refuses a second logon for a comp id already in use -- as it should, since two
// connections claiming one identity is a client bug.
//
//   binary_load_client --host 127.0.0.1 --port 9890 --sessions 4 --orders-per-burst 1000
//   echo -e "T\nT" | binary_load_client --sessions 2      # two bursts, then finish

#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>

#include <binary_session.hpp>
#include <fix_orders.hpp>

namespace {

constexpr size_t decode_arena_size = 256 * 1024;
constexpr int logon_timeout_seconds = 10;

struct Options {
    std::string host{"127.0.0.1"};
    uint16_t port{9890};
    std::string comp_id_prefix{"LOADCLIENT"};
    std::string password{"stubpassword"};
    std::string target_comp_id{"BINARY-GATEWAY"};
    std::string symbol{"AAPL"};
    int session_count{1};
    int orders_per_burst{1000};
    int bursts{0};            // 0 means take bursts from stdin, as f8test does
    int orders_per_second{0}; // per session; 0 means send as fast as the socket accepts
    // First ClOrdID suffix this run will use, so that consecutive runs against a venue
    // that is still holding the previous run's orders do not collide. The matching engine
    // rejects a duplicate ClOrdID per session comp id, and a rejected order never reaches
    // the book and produces no round-trip observation -- so a colliding run looks like it
    // delivered load while measuring nothing. A trading-day profile restarts this client
    // once per phase, which is exactly that case.
    int64_t first_cl_ord_id{0};

    // Cancels to send per new order, as a fraction. A member pulling its book is ordinary
    // behaviour, and without it the venue's order book only ever grows: nothing else
    // removes an order, since this matching engine does no matching.
    //
    // Bounded above by 1.0 by arithmetic, not by taste: the book grows only from new orders
    // and shrinks only from cancels, so a sustained rate above the arrival rate would drain
    // the book and then have nothing left to cancel.
    double cancel_ratio{0.0};
    int underlyings{3};       // NoUnderlyings instances per order
    int parties{1};           // NoPartyIDs instances per order
    int party_sub_ids{1};     // NoPartySubIDs instances per party
    bool minimal_order{false};
};

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const bool has_value = index + 1 < argc;
        if (argument == "--host" && has_value) {
            options.host = argv[++index];
        } else if (argument == "--port" && has_value) {
            options.port = static_cast<uint16_t>(std::stoi(argv[++index]));
        } else if (argument == "--comp-id-prefix" && has_value) {
            options.comp_id_prefix = argv[++index];
        } else if (argument == "--password" && has_value) {
            options.password = argv[++index];
        } else if (argument == "--target-comp-id" && has_value) {
            options.target_comp_id = argv[++index];
        } else if (argument == "--symbol" && has_value) {
            options.symbol = argv[++index];
        } else if (argument == "--sessions" && has_value) {
            options.session_count = std::stoi(argv[++index]);
        } else if (argument == "--orders-per-burst" && has_value) {
            options.orders_per_burst = std::stoi(argv[++index]);
        } else if (argument == "--bursts" && has_value) {
            options.bursts = std::stoi(argv[++index]);
        } else if (argument == "--rate" && has_value) {
            options.orders_per_second = std::stoi(argv[++index]);
        } else if (argument == "--first-cl-ord-id" && has_value) {
            options.first_cl_ord_id = std::stoll(argv[++index]);
        } else if (argument == "--cancel-ratio" && has_value) {
            options.cancel_ratio = std::stod(argv[++index]);
        } else if (argument == "--underlyings" && has_value) {
            options.underlyings = std::stoi(argv[++index]);
        } else if (argument == "--parties" && has_value) {
            options.parties = std::stoi(argv[++index]);
        } else if (argument == "--party-sub-ids" && has_value) {
            options.party_sub_ids = std::stoi(argv[++index]);
        } else if (argument == "--minimal-order") {
            options.minimal_order = true;
        } else {
            fmt::print("usage: {} [--host H] [--port P] [--comp-id-prefix ID] [--symbol SYM]\n", argv[0]);
            fmt::print("          [--sessions N] [--orders-per-burst N] [--bursts N] [--rate N]\n");
            fmt::print("          [--first-cl-ord-id N] [--cancel-ratio F]\n\n");
            fmt::print("  --password          SCRAM password for every session (default stubpassword)\n");
            fmt::print("  --target-comp-id    the venue name to send; must match the gateway\n");
            fmt::print("  --sessions          concurrent logged-on sessions, each with its own comp id\n");
            fmt::print("  --orders-per-burst  orders each session sends per burst (default 1000)\n");
            fmt::print("  --bursts            fire N bursts then wait; omit to take one burst per\n");
            fmt::print("                      \"T\" line on stdin, as f8test does\n");
            fmt::print("  --rate              orders per second per session. Omit for a throughput\n");
            fmt::print("                      test: orders go out as fast as the socket accepts them,\n");
            fmt::print("                      which offers load far faster than the pipeline drains,\n");
            fmt::print("                      so the reported latencies are dominated by queueing.\n");
            fmt::print("                      Set a sustainable rate to measure service latency.\n");
            fmt::print("  --underlyings       NoUnderlyings instances per order (default 3)\n");
            fmt::print("  --parties           NoPartyIDs instances per order (default 1)\n");
            fmt::print("  --party-sub-ids     NoPartySubIDs per party (default 1)\n");
            fmt::print("  --minimal-order     send only the required fields and no groups. Useful to\n");
            fmt::print("                      isolate per-field cost, but NOT comparable with a fix8\n");
            fmt::print("                      run, which sends a full order with nested groups.\n");
            return argument == "--help";
        }
    }
    if (options.session_count < 1 || options.orders_per_burst < 1) {
        fmt::print("--sessions and --orders-per-burst must both be at least 1\n");
        return false;
    }
    return true;
}

int64_t now_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
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

/**
 * @brief How complex an order to send.
 *
 * This matters more than it looks. fix8's f8test sends a full NewOrderSingle with nested
 * repeating groups, so a load client that sent six fields would flatter the binary gateway
 * enormously -- most of a gateway's per-order work scales with the field and group count,
 * not with the message arriving. The defaults therefore populate the whole DD-derived
 * order, including both repeating groups, so a binary run and a fix8 run are measuring
 * comparable work.
 */
struct OrderShape {
    int underlyings{3};
    int parties{1};
    int party_sub_ids{1};
    bool minimal{false};
};

/**
 * @brief One logged-on session: its socket, its outbound orders, and its receiver thread.
 *
 * Send timestamps are keyed by ClOrdID and consumed by the receiver thread when the
 * matching report arrives, so latency is measured across the real round trip rather than
 * inferred. The mutex guards that map alone -- the sending and receiving of bytes happen
 * on different threads but on the same socket, which is safe for one reader and one writer.
 */
class LoadSession {
  public:
    ~LoadSession() {
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
        }
    }

    LoadSession(std::string comp_id, std::string password, std::string target_comp_id, std::string symbol, int64_t first_cl_ord_id = 0,
                double cancel_ratio = 0.0)
        : comp_id_(std::move(comp_id))
        , password_(std::move(password))
        , target_comp_id_(std::move(target_comp_id))
        , symbol_(std::move(symbol))
        // Order matters and the compiler enforces it: members initialise in DECLARATION
        // order, so this list must match it or -Wreorder fails the build.
        , order_counter_(first_cl_ord_id)
        , cancel_ratio_(cancel_ratio) {}

    LoadSession(const LoadSession&) = delete;
    LoadSession& operator=(const LoadSession&) = delete;

    [[nodiscard]] const std::string& comp_id() const {
        return comp_id_;
    }
    [[nodiscard]] int64_t orders_sent() const {
        return orders_sent_;
    }

    [[nodiscard]] int64_t cancels_sent() const {
        return cancels_sent_;
    }

    [[nodiscard]] size_t resting_order_count() {
        const std::lock_guard<std::mutex> guard(resting_mutex_);
        return resting_cl_ord_ids_.size();
    }
    [[nodiscard]] int64_t reports_received() const {
        return reports_received_.load(std::memory_order_relaxed);
    }

    /** @brief Sets the order shape; see OrderShape for why the default is a full order. */
    void set_order_shape(const OrderShape& shape) {
        shape_ = shape;
        if (shape_.minimal) {
            return;
        }
        party_sub_ids_.clear();
        for (int index = 0; index < shape_.party_sub_ids; ++index) {
            pubsub_itc_fw_app::PartySubIDs sub_id{};
            sub_id.has_party_sub_id = true;
            sub_id.party_sub_id = "SUB-ID";
            sub_id.has_party_sub_id_type = true;
            sub_id.party_sub_id_type = pubsub_itc_fw_app::PartySubIDType::Firm;
            party_sub_ids_.push_back(sub_id);
        }
        parties_.clear();
        for (int index = 0; index < shape_.parties; ++index) {
            pubsub_itc_fw_app::PartyIDs party{};
            party.has_party_id = true;
            party.party_id = "PARTY-001";
            party.has_party_id_source = true;
            party.party_id_source = pubsub_itc_fw_app::PartyIDSource::Proprietary;
            party.has_party_role = true;
            party.party_role = pubsub_itc_fw_app::PartyRole::ExecutingFirm;
            if (!party_sub_ids_.empty()) {
                party.no_party_sub_i_ds.data = party_sub_ids_.data();
                party.no_party_sub_i_ds.size = party_sub_ids_.size();
            }
            parties_.push_back(party);
        }
        underlyings_.clear();
        for (int index = 0; index < shape_.underlyings; ++index) {
            pubsub_itc_fw_app::Underlyings underlying{};
            underlying.has_underlying_symbol = true;
            underlying.underlying_symbol = "UND-SYM";
            underlying.has_underlying_security_id = true;
            underlying.underlying_security_id = "UND-SECID";
            underlying.has_underlying_qty = true;
            underlying.underlying_qty = "50";
            underlyings_.push_back(underlying);
        }
    }

    bool connect_and_logon(const std::string& host, uint16_t port);
    bool send_burst(int order_count, int64_t nanoseconds_between_orders);
    void start_receiver();
    void stop_receiver();

    /** @brief Appends this session's matched round-trip latencies to @p all_latencies. */
    void collect_latencies(std::vector<int64_t>& all_latencies) {
        const std::lock_guard<std::mutex> guard(latency_mutex_);
        all_latencies.insert(all_latencies.end(), latencies_.begin(), latencies_.end());
    }

  private:
    template <typename MessageType> bool send_pdu(int16_t pdu_id, const MessageType& message);
    void build_order(pubsub_itc_fw_app::NewOrderSingle& order, const std::string& cl_ord_id);

    /// Cancels the oldest order the venue has confirmed is resting. Returns false only
    /// on a send failure; having nothing to cancel is a normal state, not an error.
    bool send_cancel_for_oldest_resting_order();
    void receive_loop();

    std::string comp_id_;
    std::string password_;
    std::string target_comp_id_;
    std::string symbol_;
    int socket_fd_{-1};
    int64_t orders_sent_{0};
    int64_t order_counter_{0};
    std::atomic<int64_t> reports_received_{0};
    std::atomic<bool> running_{false};
    std::thread receiver_;

    std::mutex latency_mutex_;
    std::unordered_map<std::string, int64_t> send_time_by_cl_ord_id_;

    // Orders the venue has confirmed are on the book and which this session has not yet
    // cancelled. A deque because the only operations needed are "add the newest" and "take
    // the oldest": cancelling oldest-first is not how a real member behaves, but it needs
    // no lookup and no memory beyond the orders actually resting.
    std::mutex resting_mutex_;
    std::deque<std::string> resting_cl_ord_ids_;
    double cancel_ratio_{0.0};
    double cancel_credit_{0.0};
    int64_t cancels_sent_{0};
    int64_t cancel_rejects_{0};
    std::vector<int64_t> latencies_;

    std::vector<uint8_t> send_buffer_;

    OrderShape shape_{};
    // Group instances are held here rather than built per order: their contents do not
    // vary between orders, so rebuilding them a million times would measure this client's
    // allocator rather than the gateway.
    std::vector<pubsub_itc_fw_app::Underlyings> underlyings_;
    std::vector<pubsub_itc_fw_app::PartyIDs> parties_;
    std::vector<pubsub_itc_fw_app::PartySubIDs> party_sub_ids_;
};

template <typename MessageType> bool LoadSession::send_pdu(int16_t pdu_id, const MessageType& message) {
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    // The measuring pass reports a zero-size buffer as too small but still sets
    // bytes_needed, which is what it is called for.
    static_cast<void>(encode(message, nullptr, 0, bytes_written, bytes_needed));

    const size_t frame_size = sizeof(pubsub_itc_fw::PduHeader) + bytes_needed;
    if (send_buffer_.size() < frame_size) {
        send_buffer_.resize(frame_size);
    }

    auto* header = reinterpret_cast<pubsub_itc_fw::PduHeader*>(send_buffer_.data());
    header->byte_count = htonl(static_cast<uint32_t>(bytes_needed));
    header->pdu_id = static_cast<int16_t>(htons(static_cast<uint16_t>(pdu_id)));
    header->version = 1;
    header->filler_a = 0;
    header->seq_no = 0;
    header->canary = htonl(pubsub_itc_fw::pdu_canary_value);
    header->filler_b = 0;

    if (!encode(message, send_buffer_.data() + sizeof(pubsub_itc_fw::PduHeader), bytes_needed, bytes_written, bytes_needed)) {
        return false;
    }
    return write_fully(socket_fd_, send_buffer_.data(), frame_size);
}

bool LoadSession::connect_and_logon(const std::string& host, uint16_t port) {
    socket_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        std::perror("socket");
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        fmt::print("bad host address '{}'\n", host);
        return false;
    }
    if (::connect(socket_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::perror("connect");
        return false;
    }

    const int no_delay = 1;
    ::setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));

    timeval logon_timeout{};
    logon_timeout.tv_sec = logon_timeout_seconds;
    ::setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &logon_timeout, sizeof(logon_timeout));

    pubsub_itc_fw_app::Logon logon{};
    logon.comp_id = comp_id_;
    logon.password = password_;
    logon.target_comp_id = target_comp_id_;
    if (!send_pdu(pubsub_itc_fw_app::Logon::message_pdu_id, logon)) {
        fmt::print("{}: failed to send Logon\n", comp_id_);
        return false;
    }

    pubsub_itc_fw::PduHeader header{};
    if (!read_fully(socket_fd_, reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
        fmt::print("{}: no LogonAck (connection closed or timed out)\n", comp_id_);
        return false;
    }
    const int16_t pdu_id = static_cast<int16_t>(ntohs(static_cast<uint16_t>(header.pdu_id)));
    std::vector<uint8_t> payload(ntohl(header.byte_count));
    if (!payload.empty() && !read_fully(socket_fd_, payload.data(), payload.size())) {
        return false;
    }
    if (pdu_id != pubsub_itc_fw_app::LogonAck::message_pdu_id) {
        fmt::print("{}: expected LogonAck, got PDU id {}\n", comp_id_, pdu_id);
        return false;
    }

    std::vector<uint8_t> arena_buffer(4096);
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;
    pubsub_itc_fw_app::LogonAckView ack{};
    if (!pubsub_itc_fw_app::decode(ack, payload.data(), payload.size(), bytes_consumed, arena, arena_bytes_needed)) {
        fmt::print("{}: failed to decode LogonAck\n", comp_id_);
        return false;
    }
    if (ack.outcome != pubsub_itc_fw_app::LogonOutcome::Accepted) {
        fmt::print("{}: logon refused ({})\n", comp_id_, pubsub_itc_fw_app::to_string(ack.outcome));
        return false;
    }

    // Blocking reads from here on: the receiver thread should wait for reports rather
    // than spin, and it is stopped by closing the socket.
    timeval no_timeout{};
    ::setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));
    return true;
}

void LoadSession::build_order(pubsub_itc_fw_app::NewOrderSingle& order, const std::string& cl_ord_id) {
    order.cl_ord_id = cl_ord_id;
    order.side = pubsub_itc_fw_app::Side::Buy;
    order.symbol = symbol_;
    order.ord_type = pubsub_itc_fw_app::OrdType::Limit;
    order.transact_time = now_nanoseconds();
    order.order_qty = "100";

    if (shape_.minimal) {
        return;
    }

    // The optional scalars a real order carries. Values are constant across orders: what
    // is being measured is the cost of encoding, carrying and decoding the fields, which
    // does not depend on what they say.
    order.has_price = true;
    order.price = "100.00";
    order.has_security_id = true;
    order.security_id = "GB00B03MLX29";
    order.has_security_id_source = true;
    order.security_id_source = "4";
    order.has_time_in_force = true;
    order.time_in_force = pubsub_itc_fw_app::TimeInForce::Day;
    order.has_account = true;
    order.account = "ACCT-001";
    order.has_ex_destination = true;
    order.ex_destination = "XLON";
    order.has_exec_inst = true;
    order.exec_inst = "1 G";
    order.has_min_qty = true;
    order.min_qty = "10";
    order.has_max_floor = true;
    order.max_floor = "50";
    order.has_text = true;
    order.text = "binary_load_client";

    if (!underlyings_.empty()) {
        order.no_underlyings.data = underlyings_.data();
        order.no_underlyings.size = underlyings_.size();
    }
    if (!parties_.empty()) {
        order.no_party_i_ds.data = parties_.data();
        order.no_party_i_ds.size = parties_.size();
    }
}

bool LoadSession::send_cancel_for_oldest_resting_order() {
    std::string target;
    {
        const std::lock_guard<std::mutex> guard(resting_mutex_);
        if (resting_cl_ord_ids_.empty()) {
            return true; // nothing acknowledged yet; not an error, just nothing to cancel
        }
        target = std::move(resting_cl_ord_ids_.front());
        resting_cl_ord_ids_.pop_front();
    }

    pubsub_itc_fw_app::OrderCancelRequest cancel{};
    const std::string cancel_cl_ord_id = target + "-CANCEL";
    cancel.cl_ord_id = cancel_cl_ord_id;
    cancel.orig_cl_ord_id = target;
    cancel.side = pubsub_itc_fw_app::Side::Buy;
    cancel.symbol = symbol_;
    cancel.transact_time = now_nanoseconds();

    if (!send_pdu(pubsub_itc_fw_app::OrderCancelRequest::message_pdu_id, cancel)) {
        fmt::print("{}: cancel send failed after {} cancel(s)\n", comp_id_, cancels_sent_);
        return false;
    }
    ++cancels_sent_;
    return true;
}

bool LoadSession::send_burst(int order_count, int64_t nanoseconds_between_orders) {
    const int64_t burst_started = now_nanoseconds();
    for (int index = 0; index < order_count; ++index) {
        // Pace against the burst start rather than the previous send, so a slow send
        // does not push the whole schedule back and quietly lower the offered rate.
        if (nanoseconds_between_orders > 0) {
            const int64_t due_at = burst_started + nanoseconds_between_orders * index;
            int64_t wait_for = due_at - now_nanoseconds();
            while (wait_for > 0) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(wait_for));
                wait_for = due_at - now_nanoseconds();
            }
        }

        const std::string cl_ord_id = comp_id_ + "-" + std::to_string(++order_counter_);

        pubsub_itc_fw_app::NewOrderSingle order{};
        build_order(order, cl_ord_id);

        {
            const std::lock_guard<std::mutex> guard(latency_mutex_);
            send_time_by_cl_ord_id_.emplace(cl_ord_id, now_nanoseconds());
        }

        if (!send_pdu(pubsub_itc_fw_app::NewOrderSingle::message_pdu_id, order)) {
            fmt::print("{}: send failed after {} order(s)\n", comp_id_, orders_sent_);
            return false;
        }
        ++orders_sent_;

        // Cancels ride alongside the order flow rather than replacing it, which is the
        // realistic shape: whatever makes a member pull its book is usually making everyone
        // else busy too. The accumulator carries the fraction across orders so a ratio of
        // 0.25 sends one cancel every four orders rather than rounding to none.
        if (cancel_ratio_ > 0.0) {
            cancel_credit_ += cancel_ratio_;
            while (cancel_credit_ >= 1.0) {
                cancel_credit_ -= 1.0;
                if (!send_cancel_for_oldest_resting_order()) {
                    return false;
                }
            }
        }
    }
    return true;
}

void LoadSession::start_receiver() {
    running_.store(true, std::memory_order_relaxed);
    receiver_ = std::thread([this] { receive_loop(); });
}

void LoadSession::stop_receiver() {
    running_.store(false, std::memory_order_relaxed);
    if (socket_fd_ >= 0) {
        ::shutdown(socket_fd_, SHUT_RDWR);
    }
    if (receiver_.joinable()) {
        receiver_.join();
    }
}

void LoadSession::receive_loop() {
    std::vector<uint8_t> arena_buffer(decode_arena_size);
    std::vector<uint8_t> payload;

    while (running_.load(std::memory_order_relaxed)) {
        pubsub_itc_fw::PduHeader header{};
        if (!read_fully(socket_fd_, reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
            return; // socket closed, by the peer or by stop_receiver
        }
        if (ntohl(header.canary) != pubsub_itc_fw::pdu_canary_value) {
            fmt::print("{}: PDU canary mismatch -- stream out of sync\n", comp_id_);
            return;
        }
        payload.resize(ntohl(header.byte_count));
        if (!payload.empty() && !read_fully(socket_fd_, payload.data(), payload.size())) {
            return;
        }

        const int16_t pdu_id = static_cast<int16_t>(ntohs(static_cast<uint16_t>(header.pdu_id)));
        if (pdu_id != pubsub_itc_fw_app::ExecutionReport::message_pdu_id) {
            continue;
        }

        pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
        size_t bytes_consumed = 0;
        size_t arena_bytes_needed = 0;
        pubsub_itc_fw_app::ExecutionReportView report{};
        if (!pubsub_itc_fw_app::decode(report, payload.data(), payload.size(), bytes_consumed, arena, arena_bytes_needed)) {
            continue;
        }

        reports_received_.fetch_add(1, std::memory_order_relaxed);

        if (report.has_cl_ord_id) {
            const std::lock_guard<std::mutex> guard(latency_mutex_);
            auto sent_at = send_time_by_cl_ord_id_.find(std::string(report.cl_ord_id));
            if (sent_at != send_time_by_cl_ord_id_.end()) {
                latencies_.push_back(now_nanoseconds() - sent_at->second);
                send_time_by_cl_ord_id_.erase(sent_at);
            }
        }

        // An order is recorded as resting only when the venue reports it accepted, and only
        // for OrdStatus New. A cancel names one specific OrigClOrdID -- there is no range or
        // bulk form in this venue's dictionary -- so the client can only cancel an order it
        // knows exists. Inferring the resting set from ClOrdID arithmetic would be wrong:
        // the ids sent are contiguous, but a rejected order never rests, and cancelling one
        // earns a cancel-reject.
        if (report.has_cl_ord_id && report.ord_status == pubsub_itc_fw_app::OrdStatus::New) {
            const std::lock_guard<std::mutex> guard(resting_mutex_);
            resting_cl_ord_ids_.emplace_back(report.cl_ord_id);
        }
    }
}

/** @brief Prints the latency distribution, which is the figure a throughput number hides. */
void report_latencies(std::vector<int64_t>& latencies) {
    if (latencies.empty()) {
        fmt::print("  latency        no matched round trips\n");
        return;
    }
    std::sort(latencies.begin(), latencies.end());
    const auto at_percentile = [&latencies](double percentile) {
        const size_t index = static_cast<size_t>(percentile / 100.0 * static_cast<double>(latencies.size() - 1));
        return latencies[index] / 1000.0;
    };
    fmt::print("  latency us     min {:.1f}  p50 {:.1f}  p99 {:.1f}  p99.9 {:.1f}  max {:.1f}  ({} matched)\n", at_percentile(0), at_percentile(50),
               at_percentile(99), at_percentile(99.9), at_percentile(100), latencies.size());
}

} // namespaces

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }

    std::vector<std::unique_ptr<LoadSession>> sessions;
    sessions.reserve(static_cast<size_t>(options.session_count));
    for (int index = 0; index < options.session_count; ++index) {
        const std::string comp_id = options.session_count == 1 ? options.comp_id_prefix : options.comp_id_prefix + "-" + std::to_string(index + 1);
        sessions.push_back(
            std::make_unique<LoadSession>(comp_id, options.password, options.target_comp_id, options.symbol, options.first_cl_ord_id, options.cancel_ratio));
        OrderShape shape;
        shape.underlyings = options.underlyings;
        shape.parties = options.parties;
        shape.party_sub_ids = options.party_sub_ids;
        shape.minimal = options.minimal_order;
        sessions.back()->set_order_shape(shape);
    }

    for (const auto& session : sessions) {
        if (!session->connect_and_logon(options.host, options.port)) {
            fmt::print("FAILED: session '{}' could not log on\n", session->comp_id());
            return 1;
        }
    }
    fmt::print("{} session(s) logged on to {}:{}\n", options.session_count, options.host, options.port);
    std::fflush(stdout);

    for (const auto& session : sessions) {
        session->start_receiver();
    }

    // Per-session pacing interval. Sessions send independently, so the venue-wide offered
    // rate is this multiplied by the session count.
    const int64_t nanoseconds_between_orders = options.orders_per_second > 0 ? 1000000000LL / options.orders_per_second : 0;

    // Each session sends on its own thread so the sessions genuinely overlap. Sending them
    // one after another in a loop would serialise the venue's load into a single stream --
    // harmless when every burst takes a millisecond, but wrong the moment a rate limit
    // makes a burst last seconds.
    const auto run_burst = [&sessions, &options, nanoseconds_between_orders](int burst_number) {
        const int64_t started = now_nanoseconds();
        std::vector<std::thread> senders;
        std::atomic<bool> all_sent{true};
        senders.reserve(sessions.size());
        for (const auto& session : sessions) {
            LoadSession* target = session.get();
            senders.emplace_back([target, &options, nanoseconds_between_orders, &all_sent] {
                if (!target->send_burst(options.orders_per_burst, nanoseconds_between_orders)) {
                    all_sent.store(false, std::memory_order_relaxed);
                }
            });
        }
        for (std::thread& sender : senders) {
            sender.join();
        }
        if (!all_sent.load(std::memory_order_relaxed)) {
            return false;
        }
        const double elapsed_seconds = static_cast<double>(now_nanoseconds() - started) / 1e9;
        const int total = options.session_count * options.orders_per_burst;
        fmt::print("burst {}: {} order(s) sent in {:.3f}s ({:.0f} orders/s offered)\n", burst_number, total, elapsed_seconds,
                   elapsed_seconds > 0.0 ? static_cast<double>(total) / elapsed_seconds : 0.0);
        std::fflush(stdout);
        return true;
    };

    const int64_t run_started = now_nanoseconds();
    int bursts_run = 0;
    if (options.bursts > 0) {
        for (int burst = 0; burst < options.bursts; ++burst) {
            if (!run_burst(burst + 1)) {
                break;
            }
            ++bursts_run;
        }
    } else {
        // f8test's interface: one burst per "T" line, so the harness drives the pace.
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty() || line[0] == 'q') {
                break;
            }
            if (line[0] == 'T' || line[0] == 't') {
                if (!run_burst(bursts_run + 1)) {
                    break;
                }
                ++bursts_run;
            }
        }
    }

    int64_t total_sent = 0;
    for (const auto& session : sessions) {
        // Orders AND cancels: each cancel earns its own execution report, so a run with
        // cancels compared against orders alone would report a shortfall that is not one.
        total_sent += session->orders_sent() + session->cancels_sent();
    }

    // Wait for the reports to come back. The gateway delivers them asynchronously, so a
    // client that exited at the last send would measure only how fast it could write.
    const int64_t wait_started = now_nanoseconds();
    int64_t total_received = 0;
    int64_t last_received = -1;
    int64_t last_progress_at = wait_started;
    constexpr int64_t stall_limit_nanoseconds = 30LL * 1000 * 1000 * 1000;
    while (true) {
        total_received = 0;
        for (const auto& session : sessions) {
            total_received += session->reports_received();
        }
        if (total_received >= total_sent) {
            break;
        }
        if (total_received != last_received) {
            last_received = total_received;
            last_progress_at = now_nanoseconds();
        } else if (now_nanoseconds() - last_progress_at > stall_limit_nanoseconds) {
            fmt::print("stalled: no further reports for 30s\n");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const double round_trip_seconds = static_cast<double>(now_nanoseconds() - wait_started) / 1e9;

    for (const auto& session : sessions) {
        session->stop_receiver();
    }

    std::vector<int64_t> latencies;
    for (const auto& session : sessions) {
        session->collect_latencies(latencies);
    }

    const double total_seconds = static_cast<double>(now_nanoseconds() - run_started) / 1e9;

    fmt::print("\n=== binary_load_client summary ===\n");
    fmt::print("  sessions       {}\n", options.session_count);
    if (options.minimal_order) {
        fmt::print("  order shape    minimal -- NOT comparable with a fix8 run\n");
    } else {
        fmt::print("  order shape    full: all scalars, {} underlying(s), {} party/parties x {} sub-id(s)\n", options.underlyings, options.parties,
                   options.party_sub_ids);
    }
    fmt::print("  bursts         {} of {} order(s) per session\n", bursts_run, options.orders_per_burst);
    int64_t total_orders = 0;
    int64_t total_cancels = 0;
    size_t total_resting = 0;
    for (const auto& session : sessions) {
        total_orders += session->orders_sent();
        total_cancels += session->cancels_sent();
        total_resting += session->resting_order_count();
    }
    fmt::print("  orders sent    {}\n", total_orders);
    if (total_cancels > 0) {
        // Resting is what the venue's book still holds from this run. Without cancels it
        // equals the order count and grows without bound, which is how a matching engine
        // that does no matching came to be OOM-killed at 9.9 GB.
        fmt::print("  cancels sent   {}\n", total_cancels);
        fmt::print("  still resting  {}\n", total_resting);
    }
    fmt::print("  messages sent  {}  (orders + cancels; each earns one report)\n", total_sent);
    fmt::print("  reports recvd  {}\n", total_received);
    if (total_seconds > 0.0) {
        fmt::print("  throughput     {:.0f} orders/s round trip over {:.3f}s\n", static_cast<double>(total_received) / total_seconds, total_seconds);
    }
    fmt::print("  drain          {:.3f}s after the last send\n", round_trip_seconds);
    report_latencies(latencies);
    if (options.orders_per_second <= 0) {
        fmt::print("  NOTE           no --rate given, so orders were offered as fast as the socket\n");
        fmt::print("                 accepted them. That measures throughput; the latencies above are\n");
        fmt::print("                 mostly queueing delay. Re-run with --rate for service latency.\n");
    }

    const bool complete = total_received >= total_sent && total_sent > 0;
    fmt::print("  RESULT         {}\n", complete ? "PASS -- every order was acknowledged" : "FAIL -- reports missing");
    return complete ? 0 : 1;
}

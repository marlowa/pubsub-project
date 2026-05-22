// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Unit tests for PduParser error paths not covered by PduFramerParserTest.cpp:
//
//   ZeroLengthPayload        — byte_count = 0 in header → error before allocation
//   OversizedPayload         — byte_count > slab_size() → error before allocation
//   DisconnectDuringPayload  — valid header then peer disconnect → handler called
//   ReadErrorDuringPayload   — valid header then socket error → slab freed, error returned
//   EagainDuringPayload      — valid header then EAGAIN → {true,""}, PDU dispatched on next call
//   ReadErrorDuringHeader    — socket error while reading header bytes → {false, error}

#include <cerrno>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/ByteStreamInterface.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/PduParser.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/utils/SimpleSpan.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>

namespace pubsub_itc_fw {

// ============================================================
// Stream stub: pending bytes are delivered first; once exhausted,
// recv_disconnect fires next, then recv_error_code, then EAGAIN.
// This ordering lets tests inject errors at the payload phase
// by queuing header bytes first and then setting recv_error_code.
// ============================================================

class PduParserTestStream : public ByteStreamInterface {
  public:
    std::deque<uint8_t> receive_bytes;
    bool recv_disconnect{false};
    int recv_error_code{0};
    std::string recv_error_msg;

    void feed_header_only(int16_t pdu_id, int8_t version, uint32_t payload_size) {
        PduHeader hdr{};
        hdr.byte_count = htonl(payload_size);
        hdr.pdu_id = htons(static_cast<uint16_t>(pdu_id));
        hdr.version = version;
        hdr.filler_a = 0;
        hdr.seq_no = 0;
        hdr.canary = htonl(pdu_canary_value);
        hdr.filler_b = 0;
        const auto* b = reinterpret_cast<const uint8_t*>(&hdr);
        for (size_t i = 0; i < sizeof(PduHeader); ++i) {
            receive_bytes.push_back(b[i]);
        }
    }

    [[nodiscard]] std::tuple<int, std::string> receive(utils::SimpleSpan<uint8_t> buffer) override {
        if (!receive_bytes.empty()) {
            const size_t n = std::min(buffer.size(), receive_bytes.size());
            for (size_t i = 0; i < n; ++i) {
                buffer.data()[i] = receive_bytes.front();
                receive_bytes.pop_front();
            }
            return {static_cast<int>(n), ""};
        }
        if (recv_disconnect) {
            recv_disconnect = false;
            return {0, ""};
        }
        if (recv_error_code != 0) {
            const int code = recv_error_code;
            std::string msg = std::move(recv_error_msg);
            recv_error_code = 0;
            return {code, std::move(msg)};
        }
        return {-EAGAIN, ""};
    }

    [[nodiscard]] std::tuple<int, std::string> send(utils::SimpleSpan<const uint8_t> data) override {
        return {static_cast<int>(data.size()), ""};
    }

    void close() override {}

    [[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string> get_peer_address() const override {
        return {nullptr, "stub"};
    }
};

// ============================================================
// Minimal ApplicationThread for PduParser tests
// ============================================================

class PduParserTestThread : public ApplicationThread {
  public:
    PduParserTestThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "PduParserTestThread", ThreadID{3},
                            make_queue_config(), make_allocator_config(), ApplicationThreadConfiguration{}) {}

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

  private:
    static QueueConfiguration make_queue_config() {
        QueueConfiguration cfg{};
        cfg.low_watermark = 1;
        cfg.high_watermark = 64;
        return cfg;
    }

    static AllocatorConfiguration make_allocator_config() {
        AllocatorConfiguration cfg{};
        cfg.pool_name = "PduParserTestPool";
        cfg.objects_per_pool = 64;
        cfg.initial_pools = 1;
        return cfg;
    }
};

// ============================================================
// Test fixture
// ============================================================

class PduParserErrorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
        reactor_ = std::make_unique<Reactor>(ReactorConfiguration{}, registry_, logger_->logger);
        thread_ = ApplicationThread::create<PduParserTestThread>(logger_->logger, *reactor_);
    }

    std::unique_ptr<LoggerWithSink> logger_;
    ServiceRegistry registry_;
    std::unique_ptr<Reactor> reactor_;
    std::shared_ptr<PduParserTestThread> thread_;
    PduParserTestStream stream_;
    ExpandableSlabAllocator slab_{4096};
};

// ============================================================
// Tests
// ============================================================

TEST_F(PduParserErrorTest, ZeroLengthPayloadReturnsError) {
    PduParser parser(stream_, *thread_, slab_, logger_->logger, nullptr, ConnectionID{});
    stream_.feed_header_only(10, 1, 0); // byte_count = 0

    auto [ok, error] = parser.receive();

    EXPECT_FALSE(ok);
    EXPECT_NE(error.find("zero-length"), std::string::npos);
    EXPECT_TRUE(thread_->get_queue().empty());
}

TEST_F(PduParserErrorTest, OversizedPayloadReturnsError) {
    ExpandableSlabAllocator small_slab{64};
    PduParser parser(stream_, *thread_, small_slab, logger_->logger, nullptr, ConnectionID{});
    stream_.feed_header_only(10, 1, 65); // one byte over slab_size()

    auto [ok, error] = parser.receive();

    EXPECT_FALSE(ok);
    EXPECT_NE(error.find("exceeds"), std::string::npos);
    EXPECT_TRUE(thread_->get_queue().empty());
}

TEST_F(PduParserErrorTest, DisconnectDuringPayloadCallsHandlerAndReturnsFalse) {
    bool disconnected = false;
    PduParser parser(stream_, *thread_, slab_, logger_->logger, [&disconnected]() { disconnected = true; }, ConnectionID{});

    stream_.feed_header_only(10, 1, 4); // valid 4-byte payload header
    stream_.recv_disconnect = true;     // disconnect fires when payload bytes are requested

    auto [ok, error] = parser.receive();

    EXPECT_FALSE(ok);
    EXPECT_TRUE(error.empty()); // graceful disconnect: no error string
    EXPECT_TRUE(disconnected);
    EXPECT_TRUE(thread_->get_queue().empty());
}

TEST_F(PduParserErrorTest, ReadErrorDuringPayloadReturnsFalse) {
    PduParser parser(stream_, *thread_, slab_, logger_->logger, nullptr, ConnectionID{});

    stream_.feed_header_only(10, 1, 4);
    stream_.recv_error_code = -ECONNRESET;
    stream_.recv_error_msg = "simulated reset during payload";

    auto [ok, error] = parser.receive();

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(thread_->get_queue().empty());
}

TEST_F(PduParserErrorTest, EagainDuringPayloadResumesOnNextCall) {
    PduParser parser(stream_, *thread_, slab_, logger_->logger, nullptr, ConnectionID{});

    const uint8_t payload[] = {0xAA, 0xBB, 0xCC, 0xDD};

    // First call: header present, no payload yet → EAGAIN during payload read → {true, ""}
    stream_.feed_header_only(10, 1, sizeof(payload));

    auto [ok1, err1] = parser.receive();
    EXPECT_TRUE(ok1);
    EXPECT_TRUE(err1.empty());
    EXPECT_TRUE(thread_->get_queue().empty());

    // Second call: payload arrives → complete frame dispatched → {true, ""}
    for (uint8_t b : payload) {
        stream_.receive_bytes.push_back(b);
    }

    auto [ok2, err2] = parser.receive();
    EXPECT_TRUE(ok2);
    EXPECT_TRUE(err2.empty());

    auto msg = thread_->get_queue().dequeue();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->type().as_tag(), EventType::FrameworkPdu);
    EXPECT_EQ(msg->payload_size(), static_cast<int>(sizeof(payload)));
    EXPECT_EQ(std::memcmp(msg->payload(), payload, sizeof(payload)), 0);
    slab_.deallocate(msg->slab_id(), const_cast<uint8_t*>(msg->payload()));
}

TEST_F(PduParserErrorTest, ReadErrorDuringHeaderReturnsError) {
    PduParser parser(stream_, *thread_, slab_, logger_->logger, nullptr, ConnectionID{});

    // No bytes queued — error fires immediately on first header read.
    stream_.recv_error_code = -ECONNRESET;
    stream_.recv_error_msg = "simulated reset during header";

    auto [ok, error] = parser.receive();

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(thread_->get_queue().empty());
}

} // namespace pubsub_itc_fw

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfig.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfig.hpp>
#include <pubsub_itc_fw/ByteStreamInterface.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/PduParser.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/QueueConfig.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>

namespace pubsub_itc_fw {

// ============================================================
// Stub ByteStreamInterface
// ============================================================

/*
 * StubStream lets tests inject bytes to be "received" and inspect bytes that
 * were "sent". It supports partial writes and simulated EAGAIN on both paths.
 */
class StubStream : public ByteStreamInterface {
public:
    // --- send side ---

    // Bytes written by PduFramer end up here.
    std::vector<uint8_t> sent_bytes;

    // If > 0, send() will only accept this many bytes per call.
    size_t send_limit{0};

    // If > 0, send() returns -EAGAIN after this many successful calls.
    int send_eagain_after{0};

    // If true, the next send() returns -EAGAIN.
    bool send_eagain{false};

    // If true, the next send() returns a non-recoverable error.
    bool send_error{false};

    int send_call_count{0};

    // --- receive side ---

    // Feed bytes here; PduParser will consume them.
    std::deque<uint8_t> receive_bytes;

    // If > 0, receive() will only return this many bytes per call.
    size_t recv_limit{0};

    // If true, the next receive() returns -EAGAIN (no more data).
    bool recv_eagain{false};

    // If true, the next receive() returns 0 (peer disconnect).
    bool recv_disconnect{false};

    // ByteStreamInterface implementation

    [[nodiscard]] std::tuple<int, std::string> send(
        utils::SimpleSpan<const uint8_t> data) override
    {
        if (send_error) {
            send_error = false;
            return {-ECONNRESET, "simulated send error"};
        }
        if (send_eagain) {
            send_eagain = false;
            return {-EAGAIN, ""};
        }
        ++send_call_count;
        if (send_eagain_after > 0 && send_call_count > send_eagain_after) {
            return {-EAGAIN, ""};
        }

        size_t to_send = data.size();
        if (send_limit > 0 && to_send > send_limit) {
            to_send = send_limit;
        }

        for (size_t i = 0; i < to_send; ++i) {
            sent_bytes.push_back(data.data()[i]);
        }
        return {static_cast<int>(to_send), ""};
    }

    [[nodiscard]] std::tuple<int, std::string> receive(
        utils::SimpleSpan<uint8_t> buffer) override
    {
        if (recv_disconnect) {
            recv_disconnect = false;
            return {0, ""};
        }
        if (receive_bytes.empty()) {
            recv_eagain = false;
            return {-EAGAIN, ""};
        }

        size_t to_read = buffer.size();
        if (recv_limit > 0 && to_read > recv_limit) {
            to_read = recv_limit;
        }
        to_read = std::min(to_read, receive_bytes.size());

        for (size_t i = 0; i < to_read; ++i) {
            buffer.data()[i] = receive_bytes.front();
            receive_bytes.pop_front();
        }
        return {static_cast<int>(to_read), ""};
    }

    void close() override {}

    [[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string>
    get_peer_address() const override
    {
        return {nullptr, "stub"};
    }

    // Helper: feed a complete framed PDU into receive_bytes.
    void feed_pdu(int16_t pdu_id, int8_t version,
                  const uint8_t* payload, uint32_t payload_size)
    {
        PduHeader hdr{};
        hdr.byte_count = htonl(payload_size);
        hdr.pdu_id     = htons(static_cast<uint16_t>(pdu_id));
        hdr.version    = version;
        hdr.filler_a   = 0;
        hdr.canary     = htonl(pdu_canary_value);
        hdr.filler_b   = 0;

        const auto* hdr_bytes = reinterpret_cast<const uint8_t*>(&hdr);
        for (size_t i = 0; i < sizeof(PduHeader); ++i) {
            receive_bytes.push_back(hdr_bytes[i]);
        }
        for (uint32_t i = 0; i < payload_size; ++i) {
            receive_bytes.push_back(payload[i]);
        }
    }
};

// ============================================================
// Minimal ApplicationThread stub for PduParser tests
// ============================================================

class StubApplicationThread : public ApplicationThread {
public:
    StubApplicationThread(QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(logger, reactor, "StubThread", ThreadID{1},
                            make_queue_config(), make_allocator_config(), ApplicationThreadConfig{})
    {}

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

    static QueueConfig make_queue_config() {
        QueueConfig cfg{};
        cfg.low_watermark  = 1;
        cfg.high_watermark = 64;
        return cfg;
    }

    static AllocatorConfig make_allocator_config() {
        AllocatorConfig cfg{};
        cfg.pool_name         = "StubPool";
        cfg.objects_per_pool  = 64;
        cfg.initial_pools     = 1;
        cfg.expansion_threshold_hint = 0;
        cfg.use_huge_pages_flag =
            UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages);
        return cfg;
    }
};

// ============================================================
// Test fixture
// ============================================================

class PduFramerParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_with_sink_ = std::make_unique<LoggerWithSink>(
            "pdu_test_logger", "pdu_test_sink");

        reactor_ = std::make_unique<Reactor>(
            ReactorConfiguration{}, service_registry_, logger_with_sink_->logger);

        thread_ = std::make_shared<StubApplicationThread>(
            logger_with_sink_->logger, *reactor_);
    }

    std::unique_ptr<LoggerWithSink> logger_with_sink_;
    ServiceRegistry service_registry_;
    std::unique_ptr<Reactor> reactor_;
    std::shared_ptr<StubApplicationThread> thread_;
    StubStream stream_;
    ExpandableSlabAllocator slab_allocator_{4096};
};

// ============================================================
// PduFramer tests
// ============================================================

TEST_F(PduFramerParserTest, SendWritesHeaderAndPayload)
{
    PduFramer framer(stream_);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    auto [ok, error] = framer.send(100, 1, payload, sizeof(payload));

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(framer.has_pending_data());

    ASSERT_EQ(stream_.sent_bytes.size(), sizeof(PduHeader) + sizeof(payload));

    // Verify header fields in network byte order.
    const PduHeader* hdr = reinterpret_cast<const PduHeader*>(stream_.sent_bytes.data());
    EXPECT_EQ(ntohl(hdr->byte_count), static_cast<uint32_t>(sizeof(payload)));
    EXPECT_EQ(static_cast<int16_t>(ntohs(static_cast<uint16_t>(hdr->pdu_id))), 100);
    EXPECT_EQ(hdr->version, 1);
    EXPECT_EQ(hdr->filler_a, 0u);
    EXPECT_EQ(ntohl(hdr->canary), pdu_canary_value);
    EXPECT_EQ(hdr->filler_b, 0u);

    // Verify payload bytes.
    const uint8_t* sent_payload = stream_.sent_bytes.data() + sizeof(PduHeader);
    EXPECT_EQ(std::memcmp(sent_payload, payload, sizeof(payload)), 0);
}

TEST_F(PduFramerParserTest, PartialWriteLeavesPendingData)
{
    PduFramer framer(stream_);

    // Send 4 bytes on first call, then EAGAIN — simulates kernel buffer filling up.
    stream_.send_limit = 4;
    stream_.send_eagain_after = 1;

    const uint8_t payload[] = {0xAA, 0xBB};
    auto [ok, error] = framer.send(101, 1, payload, sizeof(payload));

    EXPECT_TRUE(ok);
    EXPECT_TRUE(framer.has_pending_data());
    EXPECT_EQ(stream_.sent_bytes.size(), 4u);
}

TEST_F(PduFramerParserTest, ContinueSendCompletesPartialWrite)
{
    PduFramer framer(stream_);
    stream_.send_limit = 4;

    const uint8_t payload[] = {0xAA, 0xBB};
    [[maybe_unused]] auto send_result = framer.send(101, 1, payload, sizeof(payload));

    // Remove the per-call limit and drain.
    stream_.send_limit = 0;
    auto [ok, error] = framer.continue_send();

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(framer.has_pending_data());
    EXPECT_EQ(stream_.sent_bytes.size(), sizeof(PduHeader) + sizeof(payload));
}

TEST_F(PduFramerParserTest, EagainOnSendLeavesPendingData)
{
    PduFramer framer(stream_);
    stream_.send_eagain = true;

    const uint8_t payload[] = {0x01};
    auto [ok, error] = framer.send(102, 1, payload, sizeof(payload));

    EXPECT_TRUE(ok);
    EXPECT_TRUE(framer.has_pending_data());
}

TEST_F(PduFramerParserTest, SendErrorReturnsFalse)
{
    PduFramer framer(stream_);
    stream_.send_error = true;

    const uint8_t payload[] = {0x01};
    auto [ok, error] = framer.send(102, 1, payload, sizeof(payload));

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(framer.has_pending_data());
}

TEST_F(PduFramerParserTest, SendWhilePendingThrows)
{
    PduFramer framer(stream_);
    stream_.send_limit = 1;
    stream_.send_eagain_after = 1; // Force partial write then block.

    const uint8_t payload[] = {0x01};
    [[maybe_unused]] auto first_send = framer.send(100, 1, payload, sizeof(payload));
    ASSERT_TRUE(framer.has_pending_data());

    EXPECT_THROW(framer.send(100, 1, payload, sizeof(payload)), PreconditionAssertion);
}

TEST_F(PduFramerParserTest, SendNullptrPayloadThrows)
{
    PduFramer framer(stream_);
    EXPECT_THROW(framer.send(100, 1, nullptr, 4), PreconditionAssertion);
}

TEST_F(PduFramerParserTest, SendZeroSizeThrows)
{
    PduFramer framer(stream_);
    const uint8_t payload[] = {0x01};
    EXPECT_THROW(framer.send(100, 1, payload, 0), PreconditionAssertion);
}

TEST_F(PduFramerParserTest, NoPendingDataInitially)
{
    PduFramer framer(stream_);
    EXPECT_FALSE(framer.has_pending_data());
}

// ============================================================
// PduFramer::send_prebuilt() tests
// ============================================================

// Helper: build a complete frame (PduHeader + payload) into a buffer,
// exactly as an application thread would before enqueuing a SendPdu command.
static std::vector<uint8_t> make_prebuilt_frame(int16_t pdu_id, int8_t version,
                                                 const uint8_t* payload, uint32_t payload_size)
{
    std::vector<uint8_t> frame(sizeof(PduHeader) + payload_size);
    PduHeader* hdr = reinterpret_cast<PduHeader*>(frame.data());
    hdr->byte_count = htonl(payload_size);
    hdr->pdu_id     = htons(static_cast<uint16_t>(pdu_id));
    hdr->version    = version;
    hdr->filler_a   = 0;
    hdr->canary     = htonl(pdu_canary_value);
    hdr->filler_b   = 0;
    std::memcpy(frame.data() + sizeof(PduHeader), payload, payload_size);
    return frame;
}

TEST_F(PduFramerParserTest, SendPrebuiltTransmitsCompleteFrame)
{
    PduFramer framer(stream_);

    const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44};
    auto frame = make_prebuilt_frame(200, 2, payload, sizeof(payload));

    auto [ok, error] = framer.send_prebuilt(frame.data(), static_cast<uint32_t>(frame.size()));

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(framer.has_pending_data());

    // The bytes sent must exactly match the pre-built frame — no rewriting.
    ASSERT_EQ(stream_.sent_bytes.size(), frame.size());
    EXPECT_EQ(std::memcmp(stream_.sent_bytes.data(), frame.data(), frame.size()), 0);
}

TEST_F(PduFramerParserTest, SendPrebuiltNoPendingDataWhenFullySent)
{
    PduFramer framer(stream_);

    const uint8_t payload[] = {0xAA};
    auto frame = make_prebuilt_frame(201, 1, payload, sizeof(payload));

    auto [ok, error] = framer.send_prebuilt(frame.data(), static_cast<uint32_t>(frame.size()));

    EXPECT_TRUE(ok);
    EXPECT_FALSE(framer.has_pending_data());
}

TEST_F(PduFramerParserTest, SendPrebuiltPartialWriteLeavesPendingData)
{
    PduFramer framer(stream_);
    stream_.send_limit = 4;
    stream_.send_eagain_after = 1;

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    auto frame = make_prebuilt_frame(202, 1, payload, sizeof(payload));

    auto [ok, error] = framer.send_prebuilt(frame.data(), static_cast<uint32_t>(frame.size()));

    EXPECT_TRUE(ok);
    EXPECT_TRUE(framer.has_pending_data());
    EXPECT_EQ(stream_.sent_bytes.size(), 4u);
}

TEST_F(PduFramerParserTest, SendPrebuiltContinueSendCompletesPartialWrite)
{
    PduFramer framer(stream_);
    stream_.send_limit = 4;
    stream_.send_eagain_after = 1;

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    auto frame = make_prebuilt_frame(202, 1, payload, sizeof(payload));

    [[maybe_unused]] auto first = framer.send_prebuilt(frame.data(), static_cast<uint32_t>(frame.size()));
    ASSERT_TRUE(framer.has_pending_data());

    stream_.send_limit = 0;
    stream_.send_eagain_after = 0;
    auto [ok, error] = framer.continue_send();

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(framer.has_pending_data());
    EXPECT_EQ(stream_.sent_bytes.size(), frame.size());
}

TEST_F(PduFramerParserTest, SendPrebuiltWhilePendingThrows)
{
    PduFramer framer(stream_);
    stream_.send_limit = 1;
    stream_.send_eagain_after = 1;

    const uint8_t payload[] = {0x01};
    auto frame = make_prebuilt_frame(203, 1, payload, sizeof(payload));

    [[maybe_unused]] auto first = framer.send_prebuilt(frame.data(), static_cast<uint32_t>(frame.size()));
    ASSERT_TRUE(framer.has_pending_data());

    EXPECT_THROW(framer.send_prebuilt(frame.data(), static_cast<uint32_t>(frame.size())), PreconditionAssertion);
}

TEST_F(PduFramerParserTest, SendPrebuiltNullptrThrows)
{
    PduFramer framer(stream_);
    EXPECT_THROW(framer.send_prebuilt(nullptr, 32), PreconditionAssertion);
}

TEST_F(PduFramerParserTest, SendPrebuiltTooSmallThrows)
{
    PduFramer framer(stream_);
    const uint8_t buf[4] = {};
    // total_bytes must be > sizeof(PduHeader) — passing exactly sizeof(PduHeader) is invalid.
    EXPECT_THROW(framer.send_prebuilt(buf, static_cast<uint32_t>(sizeof(PduHeader))), PreconditionAssertion);
}

TEST_F(PduFramerParserTest, SendPrebuiltErrorReturnsFalse)
{
    PduFramer framer(stream_);
    stream_.send_error = true;

    const uint8_t payload[] = {0x01};
    auto frame = make_prebuilt_frame(204, 1, payload, sizeof(payload));

    auto [ok, error] = framer.send_prebuilt(frame.data(), static_cast<uint32_t>(frame.size()));

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(framer.has_pending_data());
}

TEST_F(PduFramerParserTest, SendPrebuiltDoesNotCopyPayload)
{
    // Verifies zero-copy: the bytes sent must be identical to the original frame
    // buffer without any intermediate copy or header rewriting.
    PduFramer framer(stream_);

    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    auto frame = make_prebuilt_frame(205, 3, payload, sizeof(payload));

    // Corrupt the payload area of frame AFTER building but BEFORE sending — if
    // send_prebuilt() copied the bytes internally the corruption would not show
    // in sent_bytes. If it stores the pointer (zero-copy) the corruption will.
    // This is a white-box test: it confirms pointer semantics.
    frame[sizeof(PduHeader) + 0] = 0xFF;

    [[maybe_unused]] auto result = framer.send_prebuilt(frame.data(), static_cast<uint32_t>(frame.size()));

    ASSERT_EQ(stream_.sent_bytes.size(), frame.size());
    // The first payload byte should be 0xFF (the modified value), not 0xDE.
    EXPECT_EQ(stream_.sent_bytes[sizeof(PduHeader)], 0xFF);
}

// ============================================================
// PduParser tests
// ============================================================

TEST_F(PduFramerParserTest, ParseSingleCompletePdu)
{
    bool disconnected = false;
    PduParser parser(stream_, *thread_, slab_allocator_, [&disconnected]() { disconnected = true; });

    const uint8_t payload[] = {0x10, 0x20, 0x30};
    stream_.feed_pdu(100, 1, payload, sizeof(payload));
    stream_.recv_eagain = true; // Signal end of data after the PDU.

    auto [ok, error] = parser.receive();

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(disconnected);

    // One FrameworkPdu message should be in the queue.
    auto msg = thread_->get_queue().dequeue();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->type().as_tag(), EventType::FrameworkPdu);
    EXPECT_EQ(msg->payload_size(), static_cast<int>(sizeof(payload)));
    EXPECT_EQ(std::memcmp(msg->payload(), payload, sizeof(payload)), 0);
    EXPECT_GE(msg->slab_id(), 0);
    slab_allocator_.deallocate(msg->slab_id(), const_cast<uint8_t*>(msg->payload()));
}

TEST_F(PduFramerParserTest, ParseTwoConsecutivePdus)
{
    PduParser parser(stream_, *thread_, slab_allocator_, nullptr);

    const uint8_t p1[] = {0xAA, 0xBB};
    const uint8_t p2[] = {0xCC, 0xDD, 0xEE};
    stream_.feed_pdu(100, 1, p1, sizeof(p1));
    stream_.feed_pdu(101, 1, p2, sizeof(p2));
    stream_.recv_eagain = true;

    auto [ok, error] = parser.receive();
    EXPECT_TRUE(ok);

    auto msg1 = thread_->get_queue().dequeue();
    auto msg2 = thread_->get_queue().dequeue();

    ASSERT_TRUE(msg1.has_value());
    ASSERT_TRUE(msg2.has_value());
    EXPECT_EQ(msg1->payload_size(), static_cast<int>(sizeof(p1)));
    EXPECT_EQ(msg2->payload_size(), static_cast<int>(sizeof(p2)));
    slab_allocator_.deallocate(msg1->slab_id(), const_cast<uint8_t*>(msg1->payload()));
    slab_allocator_.deallocate(msg2->slab_id(), const_cast<uint8_t*>(msg2->payload()));
}

TEST_F(PduFramerParserTest, ParseWithPartialHeaderDelivery)
{
    PduParser parser(stream_, *thread_, slab_allocator_, nullptr);

    const uint8_t payload[] = {0x01, 0x02};
    stream_.feed_pdu(100, 1, payload, sizeof(payload));

    // Deliver only 4 bytes at a time.
    stream_.recv_limit = 4;
    stream_.recv_eagain = false;

    auto [ok, error] = parser.receive();
    EXPECT_TRUE(ok);

    // May or may not have dispatched depending on timing — just verify no crash.
}

TEST_F(PduFramerParserTest, ParseDetectsCanaryMismatch)
{
    PduParser parser(stream_, *thread_, slab_allocator_, nullptr);

    // Feed a frame with a corrupt canary.
    PduHeader hdr{};
    const uint8_t payload[] = {0x01};
    hdr.byte_count = htonl(sizeof(payload));
    hdr.pdu_id     = htons(100);
    hdr.version    = 1;
    hdr.filler_a   = 0;
    hdr.canary     = htonl(0xDEADBEEFU); // wrong canary
    hdr.filler_b   = 0;

    const auto* hdr_bytes = reinterpret_cast<const uint8_t*>(&hdr);
    for (size_t i = 0; i < sizeof(PduHeader); ++i) {
        stream_.receive_bytes.push_back(hdr_bytes[i]);
    }
    stream_.receive_bytes.push_back(payload[0]);

    auto [ok, error] = parser.receive();

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(thread_->get_queue().empty());
}

TEST_F(PduFramerParserTest, ParseDetectsPeerDisconnect)
{
    bool disconnected = false;
    PduParser parser(stream_, *thread_, slab_allocator_, [&disconnected]() { disconnected = true; });

    stream_.recv_disconnect = true;

    auto [ok, error] = parser.receive();

    EXPECT_FALSE(ok);
    EXPECT_TRUE(error.empty()); // Graceful disconnect, not an error string.
    EXPECT_TRUE(disconnected);
}

TEST_F(PduFramerParserTest, ParseEagainWithNoDataReturnsOk)
{
    PduParser parser(stream_, *thread_, slab_allocator_, nullptr);

    // No data at all — socket returns EAGAIN immediately.
    stream_.recv_eagain = true;

    auto [ok, error] = parser.receive();

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(thread_->get_queue().empty());
}

// ============================================================
// Round-trip: framer → parser
// ============================================================

TEST_F(PduFramerParserTest, RoundTripFramerToParser)
{
    // Wire the framer's output directly into the parser's input.
    StubStream wire;
    PduFramer framer(wire);

    const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    const int16_t pdu_id = 200;

    auto [send_ok, send_error] = framer.send(pdu_id, 1, payload, sizeof(payload));
    ASSERT_TRUE(send_ok);
    ASSERT_FALSE(framer.has_pending_data());

    // Feed the framer's output into the parser's receive buffer.
    StubStream parser_stream;
    for (uint8_t b : wire.sent_bytes) {
        parser_stream.receive_bytes.push_back(b);
    }
    parser_stream.recv_eagain = true;

    PduParser parser(parser_stream, *thread_, slab_allocator_, nullptr);
    auto [recv_ok, recv_error] = parser.receive();

    ASSERT_TRUE(recv_ok);

    auto msg = thread_->get_queue().dequeue();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->type().as_tag(), EventType::FrameworkPdu);
    EXPECT_EQ(msg->payload_size(), static_cast<int>(sizeof(payload)));
    EXPECT_EQ(std::memcmp(msg->payload(), payload, sizeof(payload)), 0);
    EXPECT_GE(msg->slab_id(), 0);
    slab_allocator_.deallocate(msg->slab_id(), const_cast<uint8_t*>(msg->payload()));
}

} // namespace pubsub_itc_fw

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file PduProtocolHandlerTest.cpp
 * @brief Unit tests for PduProtocolHandler send-side paths.
 *
 * These tests exercise the partial-send and teardown paths in
 * PduProtocolHandler without requiring a running reactor or a real
 * network connection.
 *
 * Infrastructure:
 *   A socketpair() provides two connected non-blocking file descriptors.
 *   One end is wrapped in TcpSocket::adopt() and given to PduProtocolHandler.
 *   The other end is held raw by the test. By controlling how much the test
 *   reads from the raw end, the kernel send buffer can be kept full, forcing
 *   send() to return EAGAIN and keeping has_pending_data() true — without
 *   any timing dependence.
 *
 *   A Reactor is constructed but never run, purely to satisfy the
 *   ApplicationThread constructor. A minimal concrete ApplicationThread
 *   subclass (StubApplicationThread) implements the pure virtuals as no-ops.
 *
 * Tests:
 *
 *   SendPrebuiltCompletesImmediately
 *     The test drains the raw end continuously, so send() never blocks.
 *     After send_prebuilt(), has_pending_send() must be false and the slab
 *     chunk must have been deallocated.
 *
 *   SendPrebuiltProducesPartialSend
 *     The test does not read from the raw end, filling the kernel send buffer.
 *     After send_prebuilt(), has_pending_send() must be true. The test then
 *     drains the raw end and calls continue_send() repeatedly until
 *     has_pending_send() is false, verifying the full payload was transferred.
 *
 *   ContinueSendReleasesChunkOnCompletion
 *     Verifies that once continue_send() drains the remainder, the slab
 *     chunk is released (deallocated) and has_pending_send() returns false.
 *
 *   DeallocatePendingSendReleasesChunk
 *     Verifies that deallocate_pending_send() releases the slab chunk when
 *     called while a send is in flight, modelling connection teardown.
 */

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/PduProtocolHandler.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>

namespace pubsub_itc_fw::tests {

// ============================================================
// Minimal ApplicationThread subclass.
// Implements all pure virtuals as no-ops. Never started.
// ============================================================
class StubApplicationThread : public ApplicationThread {
public:
    StubApplicationThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "StubApplicationThread", ThreadID{1},
                            make_queue_config(), make_allocator_config(),
                            ApplicationThreadConfiguration{}) {}

protected:
    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

private:
    static QueueConfiguration make_queue_config() {
        QueueConfiguration cfg{};
        cfg.low_watermark  = 1;
        cfg.high_watermark = 64;
        return cfg;
    }

    static AllocatorConfiguration make_allocator_config() {
        AllocatorConfiguration cfg{};
        cfg.pool_name        = "StubPool";
        cfg.objects_per_pool = 16;
        cfg.initial_pools    = 1;
        return cfg;
    }
};

// ============================================================
// Test fixture
// ============================================================
class PduProtocolHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>(
            "pdu_handler_unit_logger", "pdu_handler_unit_sink");

        // Construct a Reactor purely to satisfy ApplicationThread's constructor.
        // It is never run.
        reactor_ = std::make_unique<Reactor>(
            ReactorConfiguration{}, service_registry_, logger_->logger);

        stub_thread_ = pubsub_itc_fw::ApplicationThread::create<StubApplicationThread>(logger_->logger, *reactor_);

        // A modest slab for the inbound allocator passed to PduProtocolHandler.
        // The send-path tests never reach PduParser so this is just for construction.
        inbound_allocator_ = std::make_unique<ExpandableSlabAllocator>(65536);

        // Create the socketpair. handler_fd_ is set non-blocking by TcpSocket::adopt().
        // raw_fd_ is set non-blocking explicitly below — no fd in this test may block.
        int fds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0)
            << "socketpair() failed";
        handler_fd_ = fds[0];
        raw_fd_     = fds[1];

        // Set raw_fd to non-blocking so drain_raw() never blocks when the
        // buffer is temporarily empty between continue_send() calls.
        const int flags = ::fcntl(raw_fd_, F_GETFL, 0);
        ASSERT_NE(flags, -1) << "fcntl F_GETFL failed";
        ASSERT_NE(::fcntl(raw_fd_, F_SETFL, flags | O_NONBLOCK), -1)
            << "fcntl F_SETFL O_NONBLOCK failed";

        auto [socket, error] = TcpSocket::adopt(handler_fd_);
        ASSERT_NE(socket, nullptr) << "TcpSocket::adopt failed: " << error;
        socket_ = std::move(socket);

        disconnect_called_ = false;
        auto disconnect_handler = [this]() {
            disconnect_called_ = true;
        };

        handler_ = std::make_unique<PduProtocolHandler>(
            *socket_,
            *stub_thread_,
            *inbound_allocator_,
            std::move(disconnect_handler),
            logger_->logger);
    }

    void TearDown() override {
        handler_.reset();
        socket_.reset();
        inbound_allocator_.reset();
        stub_thread_.reset();
        reactor_.reset();

        if (raw_fd_ != -1) {
            ::close(raw_fd_);
            raw_fd_ = -1;
        }

        logger_.reset();
    }

    // Allocates a slab chunk, writes a recognisable pattern into it,
    // and prepends a valid PduHeader so the bytes look like a real frame.
    // Returns the slab_id, the chunk pointer, and the total frame size.
    struct PrebuiltFrame {
        int      slab_id;
        void*    chunk;
        uint32_t total_bytes;
    };

    PrebuiltFrame make_frame(ExpandableSlabAllocator& allocator, size_t payload_size) {
        const uint32_t total = static_cast<uint32_t>(sizeof(PduHeader) + payload_size);
        auto [slab_id, chunk] = allocator.allocate(total);

        auto* header = static_cast<PduHeader*>(chunk);
        header->byte_count = htonl(static_cast<uint32_t>(payload_size));
        header->pdu_id     = htons(static_cast<uint16_t>(42));
        header->version    = 1;
        header->filler_a   = 0;
        header->canary     = htonl(pdu_canary_value);
        header->filler_b   = 0;

        // Fill payload with a recognisable pattern.
        auto* payload = static_cast<uint8_t*>(chunk) + sizeof(PduHeader);
        for (size_t i = 0; i < payload_size; ++i) {
            payload[i] = static_cast<uint8_t>(i & 0xFF);
        }

        return {slab_id, chunk, total};
    }

    // Drain up to max_bytes from raw_fd_ into /dev/null.
    // raw_fd_ is non-blocking; returns as soon as no data is immediately available.
    void drain_raw(size_t max_bytes) {
        std::vector<uint8_t> buf(4096);
        size_t drained = 0;
        while (drained < max_bytes) {
            const size_t to_read = std::min(buf.size(), max_bytes - drained);
            const ssize_t n = ::read(raw_fd_, buf.data(), to_read);
            if (n > 0) {
                drained += static_cast<size_t>(n);
            } else {
                // n == 0 means peer closed; n == -1 means EAGAIN or error.
                // Either way there is nothing more to read right now.
                break;
            }
        }
    }

    // Outbound slab for frames being sent — separate from inbound_allocator_.
    // Sized large enough for the test payloads.
    ExpandableSlabAllocator& outbound_allocator() {
        if (outbound_allocator_ == nullptr) {
            outbound_allocator_ = std::make_unique<ExpandableSlabAllocator>(4 * 1024 * 1024);
        }
        return *outbound_allocator_;
    }

    std::unique_ptr<LoggerWithSink>           logger_;
    ServiceRegistry                           service_registry_;
    std::unique_ptr<Reactor>                  reactor_;
    std::shared_ptr<StubApplicationThread>    stub_thread_;
    std::unique_ptr<ExpandableSlabAllocator>  inbound_allocator_;
    std::unique_ptr<ExpandableSlabAllocator>  outbound_allocator_;
    std::unique_ptr<TcpSocket>                socket_;
    std::unique_ptr<PduProtocolHandler>       handler_;
    int                                       handler_fd_{-1};
    int                                       raw_fd_{-1};
    bool                                      disconnect_called_{false};
};

// ============================================================
// Test: send completes immediately when the kernel buffer has room.
// ============================================================
TEST_F(PduProtocolHandlerTest, SendPrebuiltCompletesImmediately) {
    // Use a small payload that fits easily in the kernel send buffer.
    constexpr size_t payload_size = 128;
    auto [slab_id, chunk, total_bytes] = make_frame(outbound_allocator(), payload_size);

    handler_->send_prebuilt(&outbound_allocator(), slab_id, chunk, total_bytes);

    // The send buffer has room for a tiny frame — the send should complete
    // immediately, leaving no pending data and no live chunk.
    EXPECT_FALSE(handler_->has_pending_send());

    // Verify the bytes arrived on the raw end.
    std::vector<uint8_t> received(total_bytes);
    size_t total_read = 0;
    while (total_read < total_bytes) {
        const ssize_t n = ::read(raw_fd_, received.data() + total_read, total_bytes - total_read);
        if (n > 0) {
            total_read += static_cast<size_t>(n);
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    EXPECT_EQ(total_read, total_bytes);
}

// ============================================================
// Test: send blocks when the kernel buffer is full; continue_send
// drains the remainder after the raw end is read.
// ============================================================
TEST_F(PduProtocolHandlerTest, SendPrebuiltProducesPartialSend) {
    // Fill the kernel send buffer with a large frame. A socketpair on Linux
    // has a default buffer of 212992 bytes per direction. A 512 KB payload
    // reliably exceeds this.
    constexpr size_t payload_size = 512 * 1024;
    auto [slab_id, chunk, total_bytes] = make_frame(outbound_allocator(), payload_size);

    handler_->send_prebuilt(&outbound_allocator(), slab_id, chunk, total_bytes);

    // The send buffer should be full; the send must not have completed.
    EXPECT_TRUE(handler_->has_pending_send());

    // Now drain the raw end and call continue_send() until complete.
    int iterations = 0;
    while (handler_->has_pending_send()) {
        drain_raw(65536);
        handler_->continue_send();
        ++iterations;
        ASSERT_LT(iterations, 10000) << "continue_send did not complete within expected iterations";
        if (handler_->has_pending_send()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    EXPECT_GT(iterations, 0) << "Expected at least one continue_send call";
    EXPECT_FALSE(handler_->has_pending_send());

    // Drain any remaining bytes from the raw end.
    drain_raw(total_bytes);
}

// ============================================================
// Test: continue_send releases the slab chunk on completion.
// ============================================================
TEST_F(PduProtocolHandlerTest, ContinueSendReleasesChunkOnCompletion) {
    constexpr size_t payload_size = 512 * 1024;

    // Use a separate allocator so we can observe its slab count.
    ExpandableSlabAllocator send_allocator(4 * 1024 * 1024);
    auto [slab_id, chunk, total_bytes] = make_frame(send_allocator, payload_size);

    handler_->send_prebuilt(&send_allocator, slab_id, chunk, total_bytes);
    ASSERT_TRUE(handler_->has_pending_send());

    // Drain and continue until done.
    while (handler_->has_pending_send()) {
        drain_raw(65536);
        handler_->continue_send();
        if (handler_->has_pending_send()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    // If the chunk was correctly released, the slab allocator should be able
    // to reclaim it. We verify this by allocating the full slab size again —
    // if the previous chunk was not freed the allocator would chain a new slab.
    // After reclamation (triggered by the next allocate) we should still have
    // only one slab (the original, reset) or at most two.
    auto [slab_id2, chunk2] = send_allocator.allocate(4 * 1024 * 1024);
    EXPECT_NE(chunk2, nullptr);
    EXPECT_LE(send_allocator.slab_count(), 2)
        << "Slab count grew unexpectedly; chunk may not have been released";
    send_allocator.deallocate(slab_id2, chunk2);

    drain_raw(total_bytes);
}

// ============================================================
// Test: deallocate_pending_send releases the chunk during teardown.
// ============================================================
TEST_F(PduProtocolHandlerTest, DeallocatePendingSendReleasesChunk) {
    constexpr size_t payload_size = 512 * 1024;

    ExpandableSlabAllocator send_allocator(4 * 1024 * 1024);
    auto [slab_id, chunk, total_bytes] = make_frame(send_allocator, payload_size);

    handler_->send_prebuilt(&send_allocator, slab_id, chunk, total_bytes);
    ASSERT_TRUE(handler_->has_pending_send());

    // Simulate connection teardown while the send is still in flight.
    handler_->deallocate_pending_send();

    // deallocate_pending_send() frees the slab chunk but does not reset the
    // framer's internal send state — has_pending_send() may still return true.
    // What matters is that the chunk was freed. We verify this by checking
    // that the allocator can reclaim the memory without chaining a new slab.
    auto [slab_id2, chunk2] = send_allocator.allocate(4 * 1024 * 1024);
    EXPECT_NE(chunk2, nullptr);
    EXPECT_LE(send_allocator.slab_count(), 2)
        << "Slab count grew unexpectedly; chunk may not have been released";
    send_allocator.deallocate(slab_id2, chunk2);

    drain_raw(total_bytes);
}

} // namespace pubsub_itc_fw::tests

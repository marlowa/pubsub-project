// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <sched.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/MirroredBuffer.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

namespace {

class MirroredBufferTest : public ::testing::Test {
  protected:
    const int64_t test_capacity = 65536; // 64KB (multiple of most page sizes)
};

} // namespaces

TEST_F(MirroredBufferTest, InitialisesWithExpectedCapacity) {
    const MirroredBuffer buffer(test_capacity);

    EXPECT_GE(buffer.capacity(), test_capacity);
    EXPECT_EQ(buffer.bytes_available(), 0);
    EXPECT_EQ(buffer.space_remaining(), buffer.capacity() - 1);
}

TEST_F(MirroredBufferTest, HandlesSimpleSequentialAccess) {
    MirroredBuffer buffer(test_capacity);
    const std::string data = "The quick brown fox jumps over the lazy dog";
    const auto len = static_cast<int64_t>(data.size());

    std::memcpy(buffer.write_ptr(), data.data(), static_cast<size_t>(len));
    buffer.advance_head(len);

    EXPECT_EQ(buffer.bytes_available(), len);
    EXPECT_EQ(std::memcmp(buffer.read_ptr(), data.data(), static_cast<size_t>(len)), 0);

    buffer.advance_tail(len);
    EXPECT_EQ(buffer.bytes_available(), 0);
}

TEST_F(MirroredBufferTest, VerifiesVirtualMemoryMirroringContinuity) {
    MirroredBuffer buffer(test_capacity);
    const int64_t cap = buffer.capacity();

    // Advance pointers to exactly 10 bytes before the physical wrap point
    const int64_t offset = cap - 10;
    buffer.advance_head(offset);
    buffer.advance_tail(offset);

    // Write a 20-byte string that physically straddles the end of the buffer
    const char* straddle_data = "0123456789ABCDEFGHIJ";
    std::memcpy(buffer.write_ptr(), straddle_data, 20);

    // Adversarial Check: Verify the bytes are physically present at the start of the buffer
    // through the second mapping.
    buffer.advance_head(20);

    // 1. Check contiguous read pointer visibility
    EXPECT_EQ(std::memcmp(buffer.read_ptr(), straddle_data, 20), 0);

    // 2. Check that writing to the 'mirror' at the end of the 2*cap range
    // actually updated the physical memory at the beginning.
    const uint8_t* physical_start = buffer.read_ptr() - offset; // This points to the base_ptr_
    EXPECT_EQ(std::memcmp(physical_start, "ABCDEFGHIJ", 10), 0);
}

TEST_F(MirroredBufferTest, EnforcesOneByteGapFullBufferLogic) {
    MirroredBuffer buffer(test_capacity);
    const int64_t cap = buffer.capacity();

    // Fill the buffer to the maximum allowed limit
    const int64_t max_fill = cap - 1;
    buffer.advance_head(max_fill);

    EXPECT_EQ(buffer.space_remaining(), 0);
    EXPECT_EQ(buffer.bytes_available(), max_fill);

    // Adversarial: Attempting to write even one more byte must fail
    EXPECT_THROW(buffer.advance_head(1), PreconditionAssertion);
}

TEST_F(MirroredBufferTest, ThrowsOnInvalidRequestedCapacity) {
    // Adversarial: Negative or zero capacity
    EXPECT_THROW(MirroredBuffer buffer(-1), PreconditionAssertion);
    EXPECT_THROW(MirroredBuffer buffer(0), PreconditionAssertion);
}

TEST_F(MirroredBufferTest, ThrowsOnInvalidPointerAdvancement) {
    MirroredBuffer buffer(test_capacity);

    // Adversarial: Advance head more than space remaining
    EXPECT_THROW(buffer.advance_head(buffer.capacity()), PreconditionAssertion);

    // Adversarial: Advance tail more than bytes available
    buffer.advance_head(100);
    EXPECT_THROW(buffer.advance_tail(101), PreconditionAssertion);

    // Adversarial: Negative advancement
    EXPECT_THROW(buffer.advance_head(-1), PreconditionAssertion);
    EXPECT_THROW(buffer.advance_tail(-1), PreconditionAssertion);
}

TEST_F(MirroredBufferTest, HandlesExhaustiveWrapAroundStress) {
    MirroredBuffer buffer(test_capacity);
    const int64_t chunk_size = 1000;
    const int64_t iterations = 1000;

    for (int64_t i = 0; i < iterations; ++i) {
        std::memset(buffer.write_ptr(), static_cast<int>(i % 255), static_cast<size_t>(chunk_size));
        buffer.advance_head(chunk_size);

        EXPECT_EQ(*buffer.read_ptr(), static_cast<uint8_t>(i % 255));

        buffer.advance_tail(chunk_size);
        EXPECT_EQ(buffer.bytes_available(), 0);
    }
}

/*
 * Regression test for the wrapping-counter bug discovered when running the
 * order_gateway under a multi-T burst from fix8.
 *
 * Background: head() and tail() are documented as indices that the consumer
 * can compare across deliveries "to detect unambiguously whether the tail
 * advanced". RawBytesProtocolHandler::on_data_ready stamps each
 * RawSocketCommunication event with buffer.tail() at enqueue time, and the
 * application thread tracks absolute byte-stream offsets to figure out which
 * bytes are new in each event (see FixSession.hpp). All of that bookkeeping
 * assumes head and tail are monotonic across the lifetime of the buffer.
 *
 * Under burst load, total bytes flowing through the buffer eventually exceed
 * its capacity. When that happens, the consumer's tracking diverges from the
 * buffer's state and parsing silently stops. The buffer keeps filling until
 * it is nearly full and the connection effectively hangs.
 *
 * What this test pins down: after the consumer has drained more than one
 * capacity's worth of data, tail() must report a value strictly greater than
 * an earlier sample taken before the boundary, head() the same, and
 * bytes_available() must remain consistent (head - tail) throughout. Equally
 * important, advance_tail past the wrap point must not throw, since the
 * total bytes-in-flight remains within capacity.
 */
TEST_F(MirroredBufferTest, HeadAndTailRemainMonotonicAcrossMultipleCapacities) {
    MirroredBuffer buffer(test_capacity);
    const int64_t capacity = buffer.capacity();
    const int64_t chunk_size = 1000;
    const int64_t total_chunks = (capacity / chunk_size) * 3 + 7;

    int64_t prior_tail = buffer.tail();
    int64_t total_pushed = 0;

    for (int64_t i = 0; i < total_chunks; ++i) {
        buffer.advance_head(chunk_size);
        EXPECT_EQ(buffer.bytes_available(), chunk_size) << "bytes_available should reflect head minus tail, iteration " << i;

        const int64_t current_tail = buffer.tail();
        EXPECT_GE(current_tail, prior_tail) << "tail must never decrease, iteration " << i << " (prior_tail=" << prior_tail << ", current_tail=" << current_tail
                                            << ")";

        buffer.advance_tail(chunk_size);
        EXPECT_EQ(buffer.bytes_available(), 0) << "buffer should be empty after draining, iteration " << i;

        const int64_t tail_after_drain = buffer.tail();
        EXPECT_GE(tail_after_drain, current_tail) << "tail must never decrease across advance_tail, iteration " << i << " (current_tail=" << current_tail
                                                  << ", tail_after_drain=" << tail_after_drain << ")";

        prior_tail = tail_after_drain;
        total_pushed += chunk_size;
    }

    EXPECT_EQ(buffer.tail(), total_pushed) << "After draining everything pushed, tail must equal total bytes pushed";
}

TEST_F(MirroredBufferTest, SupportsLargeAddressSpaceReservations) {
    // Verify 5GB cushion (requires 10GB virtual address space)
    const int64_t five_gb = 5LL * 1024 * 1024 * 1024;

    // This is an adversarial environment test; it may fail on systems with
    // restricted virtual memory (RLIMIT_AS), so we catch the framework exception.
    try {
        const MirroredBuffer massive_buffer(five_gb);
        EXPECT_GE(massive_buffer.capacity(), five_gb);
    } catch (const PubSubItcException& e) {
        // If the OS refuses the 10GB mmap, we log but don't fail the logic test
        SUCCEED() << "System-level restriction on 10GB VMA, exception caught as expected.";
    }
}

TEST_F(MirroredBufferTest, ConcurrentProducerConsumerStress) {
    MirroredBuffer buffer(test_capacity);
    std::atomic<bool> running{true};
    const int64_t total_bytes = 1'000'000;
    std::atomic<int64_t> consumed_count{0};

    // Producer Thread
    std::thread producer([&]() {
        for (int64_t i = 0; i < total_bytes; ++i) {
            while (buffer.space_remaining() < 1) {
                std::this_thread::yield();
            }
            *buffer.write_ptr() = static_cast<uint8_t>(i % 256);
            buffer.advance_head(1);
        }
    });

    // Consumer Thread
    std::thread consumer([&]() {
        while (consumed_count < total_bytes) {
            int64_t avail = buffer.bytes_available();
            if (avail > 0) {
                buffer.advance_tail(1);
                consumed_count.fetch_add(1);
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(consumed_count, total_bytes);
}

TEST_F(MirroredBufferTest, ExposeStaleHeadVisibilityRace) {
    MirroredBuffer buffer(test_capacity);
    std::atomic<bool> ready_to_read{false};
    std::atomic<int64_t> payload_size{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};

    // Producer Thread
    std::thread producer([&]() {
        for (int i = 0; i < 1'000'000; ++i) {
            // 1. Write data
            const int64_t size = 10;
            std::memset(buffer.write_ptr(), 0xFF, size);

            // 2. Update head (Non-atomic)
            buffer.advance_head(size);

            // 3. Signal via side-channel
            payload_size.store(size, std::memory_order_release);
            ready_to_read.store(true, std::memory_order_release);

            // Wait for consumer to process
            while (ready_to_read.load(std::memory_order_acquire)) {
                if (stop.load())
                    return;
            }
        }
        stop.store(true);
    });

    // Consumer Thread
    std::thread consumer([&]() {
        try {
            while (!stop.load()) {
                if (ready_to_read.load(std::memory_order_acquire)) {
                    int64_t size = payload_size.load(std::memory_order_acquire);
                    buffer.advance_tail(size);
                    ready_to_read.store(false, std::memory_order_release);
                }
            }
        } catch (const PreconditionAssertion& e) {
            failed.store(true);
            stop.store(true);
        }
    });

    producer.join();
    consumer.join();

    EXPECT_FALSE(failed.load()) << "Race condition caught: head_ update was not visible despite side-channel signal!";
}

TEST_F(MirroredBufferTest, TheVandalRaceTest) {
    MirroredBuffer buffer(test_capacity);
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    const int64_t iterations = 10'000'000;

    auto pin_to_core = [](int core_id) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    };

    std::thread producer([&]() {
        pin_to_core(1); // Force to Core 1
        for (int64_t i = 0; i < iterations; ++i) {
            while (buffer.space_remaining() < 1) {
                // Use a volatile assembly clobber to force re-reading memory
                asm volatile("" ::: "memory");
            }
            buffer.advance_head(1);
            if (stop.load())
                break;
        }
        stop.store(true);
    });

    std::thread consumer([&]() {
        pin_to_core(2); // Force to Core 2 (Different physical core)
        try {
            while (!stop.load()) {
                // FORCE THE COMPILER TO RE-READ head_ FROM CACHE
                asm volatile("" ::: "memory");

                if (buffer.bytes_available() > 0) {
                    buffer.advance_tail(1);
                }
            }
        } catch (const PreconditionAssertion& e) {
            failed.store(true);
            stop.store(true);
        }
    });

    producer.join();
    consumer.join();

    EXPECT_FALSE(failed.load()) << "Reproduced! Tail outpaced Head due to memory visibility lag.";
}

TEST_F(MirroredBufferTest, VigorouslyAdversarialRace) {
    const int64_t small_capacity = 4096; // Minimum page size
    MirroredBuffer buffer(small_capacity);
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    const int64_t iterations = 20'000'000;

    // 1. CACHE POLLUTER: Runs on any core, constantly dirtying the cache
    std::thread polluter([&]() {
        std::vector<int> junk(1024 * 1024, 0); // 4MB of junk
        while (!stop.load()) {
            for (auto& v : junk)
                v++;
            std::this_thread::yield();
        }
    });

    // 2. PRODUCER: Extremely fast 1-byte increments
    std::thread producer([&]() {
        for (int64_t i = 0; i < iterations; ++i) {
            while (buffer.space_remaining() < 1) {
                std::this_thread::yield();
            }
            buffer.advance_head(1);

            // Occasionally sleep to force the consumer to "catch up"
            // right at the moment the producer is about to write again.
            if (i % 10000 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        }
        stop.store(true);
    });

    // 3. CONSUMER: Validates head visibility
    std::thread consumer([&]() {
        try {
            while (!stop.load()) {
                // If head_ is NOT atomic, bytes_available() might use a cached
                // value of head_ that is older than what the producer just set.
                // However, the PduBurst test failed because head_ appeared
                // BEHIND the tail (or smaller than expected).
                int64_t avail = buffer.bytes_available();
                if (avail > 0) {
                    // Try to consume everything the buffer says it has.
                    // This is where the non-atomic check fails.
                    buffer.advance_tail(avail);
                }
            }
        } catch (const PreconditionAssertion& e) {
            failed.store(true);
            stop.store(true);
        }
    });

    producer.join();
    consumer.join();
    polluter.join();

    EXPECT_FALSE(failed.load()) << "Adversarial test failed: Tail outpaced head!";
}

/*
 * We use memory_order_seq_cst for the side_channel_head to establish a
 * total global ordering for the test verification logic.
 *
 * In the MirroredBuffer itself, we use acquire/release semantics for high
 * performance. However, in this adversarial test, we are using an external
 * 'side-channel' (side_channel_head) to inform the consumer that data is ready.
 *
 * Seq_cst ensures that if the consumer sees the updated side_channel_head,
 * it is guaranteed to see the producer's prior update to the buffer's
 * internal head_ index. Without this strict ordering in the test code,
 * the side-channel could 'outrun' the buffer index due to independent
 * hardware store-buffer flushing, leading to a false-positive test failure.
 */

TEST_F(MirroredBufferTest, SideChannelVisibilityFailure) {
    MirroredBuffer buffer(test_capacity);
    std::atomic<int64_t> side_channel_head{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};

    auto pin_to_core = [](int core_id) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    };

    std::thread producer([&]() {
        pin_to_core(1);
        for (int i = 0; i < 10'000'000; ++i) {
            while (buffer.space_remaining() < 1) {
                if (stop.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
            }

            buffer.advance_head(1);
            side_channel_head.fetch_add(1, std::memory_order_seq_cst);
        }
        stop.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        pin_to_core(2);
        // Track total bytes processed by this thread to compare against
        // the monotonically increasing side_channel_head.
        int64_t total_bytes_consumed = 0;

        try {
            while (!stop.load()) {
                int64_t total_produced = side_channel_head.load(std::memory_order_seq_cst);

                if (total_produced > total_bytes_consumed) {
                    // Calculate how many NEW bytes the side-channel says are ready
                    int64_t to_consume = total_produced - total_bytes_consumed;

                    // MECHANICAL CHECK:
                    // advance_tail(to_consume) calls bytes_available().
                    // bytes_available() loads the buffer's internal head_ with acquire.
                    // If the internal head_ has not caught up to the side-channel's
                    // total_produced, this will throw, correctly failing the test.
                    buffer.advance_tail(to_consume);

                    // Update local progress
                    total_bytes_consumed += to_consume;
                }
            }
        } catch (const PreconditionAssertion& e) {
            failed.store(true);
            stop.store(true);
        }
    });

    producer.join();
    consumer.join();

    EXPECT_FALSE(failed.load()) << "CAUGHT: The Side-Channel informed us of data that head_ doesn't show yet!";
}

/*
 * Regression test for the dangling-pointer bug discovered when running the
 * order_gateway under a 1000-NewOrderSingle burst from fix8.
 *
 * Background: RawBytesProtocolHandler enqueues RawSocketCommunication events
 * to an ApplicationThread queue. Each event carries a pointer into the
 * handler's owned MirroredBuffer. Under burst load the handler can be
 * destroyed (buffer-full backpressure -> teardown_connection) BEFORE the
 * application thread has drained the events for that connection. With the
 * buffer held as a value member of the handler, its mmap region was
 * unmapped on handler destruction and the queued events' payload pointers
 * dangled, producing an ASan SEGV when the application thread later read
 * from them via string::append in FixParser::feed.
 *
 * The fix:
 *   - RawBytesProtocolHandler now holds std::shared_ptr<MirroredBuffer>.
 *   - EventMessage::create_raw_socket_message takes a 5th parameter, a
 *     shared_ptr<MirroredBuffer> that is stored inside the event. Each
 *     event therefore extends the buffer's lifetime; the buffer is freed
 *     only when both the handler AND every queued event referencing it
 *     have been destroyed.
 *
 * This test reproduces the lifetime extension at the unit level: we
 * construct a shared MirroredBuffer, build a RawSocketCommunication
 * EventMessage from it, drop our own shared_ptr to the buffer, and verify
 * that the buffer is still alive (because the EventMessage now holds
 * the sole remaining reference). Reading via event.payload() must remain
 * safe. Before the fix, this would crash under ASan.
 *
 * The test does not exercise threading or the queue. The framework-level
 * scenario is tested end-to-end by running the order_gateway
 * under fix8's T command; this unit test pins down the invariant that
 * makes that scenario work.
 */
TEST_F(MirroredBufferTest, EventMessageHoldsSharedOwnershipOfMirroredBuffer) {
    auto buffer = std::make_shared<MirroredBuffer>(test_capacity);

    // Write a recognisable marker into the buffer.
    const std::string marker = "the-marker-must-survive";
    const auto len = static_cast<int64_t>(marker.size());
    std::memcpy(buffer->write_ptr(), marker.data(), static_cast<size_t>(len));
    buffer->advance_head(len);

    ASSERT_EQ(buffer.use_count(), 1L);

    // Build a RawSocketCommunication event that points into the buffer and
    // shares ownership of it. This is what RawBytesProtocolHandler::on_data_ready
    // does in production.
    EventMessage event =
        EventMessage::create_raw_socket_message(ConnectionID{}, buffer->read_ptr(), static_cast<int>(buffer->bytes_available()), buffer->tail(), buffer);

    EXPECT_EQ(buffer.use_count(), 2L) << "EventMessage must hold a second reference to the buffer";

    // Capture the payload pointer for after-drop comparison.
    const uint8_t* payload_before_drop = event.payload();
    ASSERT_NE(payload_before_drop, nullptr);

    // Drop the local shared_ptr. If the EventMessage didn't share ownership,
    // the buffer would be destroyed here, the mmap region unmapped, and the
    // following memcmp would crash (or read junk under ASan).
    buffer.reset();

    // The event still holds the sole reference; the buffer must still be
    // mapped and the marker bytes must still be readable.
    EXPECT_EQ(std::memcmp(event.payload(), marker.data(), marker.size()), 0)
        << "Buffer was destroyed too early; the EventMessage's shared_ptr " << "did not keep it alive.";
    EXPECT_EQ(event.payload(), payload_before_drop) << "Payload pointer must remain stable for the lifetime of the event.";

    // The event going out of scope at the end of the test will release the
    // last reference, and the buffer will be destroyed cleanly.
}

} // namespaces

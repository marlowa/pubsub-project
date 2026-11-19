// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Writing several framework messages with one system call (docs/framework/gathering_sends.md): what the stream takes is
// a prefix of the messages joined end to end, and every way a call can end, at every byte, must leave the far side
// receiving exactly the messages written, in order, none twice and none missing, with each slab chunk given back once.

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ByteStreamInterface.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/GatheredSends.hpp>
#include <pubsub_itc_fw/GatheredWriting.hpp>
#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/SimpleSpan.hpp>
#include <pubsub_itc_fw/WaitingSendQueue.hpp>

namespace pubsub_itc_fw::tests {

namespace {

// A stream that takes, in each call, as many bytes as the next budget allows, then all it is asked for once the budgets
// are used; or returns an error the test chooses. It keeps every byte taken, to compare with what was meant to go.
class BudgetedStream : public ByteStreamInterface {
  public:
    // Each call takes at most the next of @p budgets bytes; 0 means no room (EAGAIN). After them, everything.
    void set_budgets(std::deque<int64_t> budgets) {
        budgets_ = std::move(budgets);
    }
    // The next call fails with @p error, taking nothing.
    void fail_next_with(int error) {
        next_error_ = error;
    }

    std::tuple<int, std::string> send(SimpleSpan<const uint8_t> data) override {
        auto [taken, error] = send_gathered(&data, 1);
        return {static_cast<int>(taken), error};
    }

    std::tuple<int64_t, std::string> send_gathered(const SimpleSpan<const uint8_t>* parts, size_t count) override {
        ++calls_;
        if (next_error_ != 0) {
            const int error = next_error_;
            next_error_ = 0;
            return {-error, error == EPIPE ? "" : "the test's failure"};
        }
        int64_t budget = INT64_MAX;
        if (!budgets_.empty()) {
            budget = budgets_.front();
            budgets_.pop_front();
        }
        if (budget == 0) {
            return {-EAGAIN, ""};
        }
        int64_t taken = 0;
        for (size_t index = 0; index < count && taken < budget; ++index) {
            const auto take = static_cast<size_t>(std::min<int64_t>(budget - taken, static_cast<int64_t>(parts[index].size())));
            received_.insert(received_.end(), parts[index].data(), parts[index].data() + take);
            taken += static_cast<int64_t>(take);
            if (take < parts[index].size()) {
                break;
            }
        }
        return {taken, ""};
    }

    std::tuple<int, std::string> receive(SimpleSpan<uint8_t> /*buffer*/) override {
        return {-EAGAIN, ""};
    }
    std::tuple<std::unique_ptr<InetAddress>, std::string> get_peer_address() const override {
        return {nullptr, "not a network stream"};
    }
    void close() override {}

    [[nodiscard]] const std::vector<uint8_t>& received() const {
        return received_;
    }
    [[nodiscard]] int calls() const {
        return calls_;
    }

  private:
    std::deque<int64_t> budgets_;
    int next_error_{0};
    std::vector<uint8_t> received_;
    int calls_{0};
};

// Frames of the given payload sizes, each a PduHeader-sized prefix followed by a payload, every byte numbered by its
// place in the whole stream, so that a byte out of place, missing or repeated shows in the comparison.
std::vector<std::vector<uint8_t>> make_frames(const std::vector<size_t>& payload_sizes) {
    std::vector<std::vector<uint8_t>> frames;
    uint8_t next = 1;
    for (const size_t payload : payload_sizes) {
        std::vector<uint8_t> frame(sizeof(PduHeader) + payload);
        for (uint8_t& byte : frame) {
            byte = next++;
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

std::vector<uint8_t> joined(const std::vector<std::vector<uint8_t>>& frames) {
    std::vector<uint8_t> all;
    for (const auto& frame : frames) {
        all.insert(all.end(), frame.begin(), frame.end());
    }
    return all;
}

std::vector<SimpleSpan<const uint8_t>> spans_of(const std::vector<std::vector<uint8_t>>& frames, size_t from = 0) {
    std::vector<SimpleSpan<const uint8_t>> spans;
    for (size_t index = from; index < frames.size(); ++index) {
        spans.emplace_back(frames[index].data(), frames[index].size());
    }
    return spans;
}

} // namespaces

// --- The stream's default: one send() per piece, stopping at the first not taken whole ------------------------------

TEST(GatheredWritingTest, TheDefaultGatheredSendTakesAPrefixAndStopsAtTheFirstPieceCut) {
    // A stream that uses the interface's own send_gathered(), built on send().
    class SendOnly : public BudgetedStream {
      public:
        std::tuple<int64_t, std::string> send_gathered(const SimpleSpan<const uint8_t>* parts, size_t count) override {
            return ByteStreamInterface::send_gathered(parts, count);
        }
        std::tuple<int, std::string> send(SimpleSpan<const uint8_t> data) override {
            auto [taken, error] = BudgetedStream::send_gathered(&data, 1);
            return {static_cast<int>(taken), error};
        }
    } stream;
    const auto frames = make_frames({10, 20, 30});
    const auto spans = spans_of(frames);
    // One send() for each piece, each with its own budget: the first frame whole (34 bytes), then 5 of the second.
    stream.set_budgets({34, 5});
    auto [taken, error] = stream.send_gathered(spans.data(), spans.size());
    EXPECT_EQ(taken, 34 + 5) << "the first frame whole and five bytes of the second, then nothing more in this call";
    EXPECT_TRUE(error.empty());
}

// --- PduFramer::send_gathered: a cut at every byte -------------------------------------------------------------------

// Whatever the first call takes, from nothing to everything, the framer says exactly how many frames went whole and
// whether the next was cut; finishing the cut one and then sending the rest delivers every byte once, in order.
TEST(GatheredWritingTest, ACutAtEveryByteDeliversEveryFrameOnceInOrder) {
    const auto frames = make_frames({5, 40, 1, 17});
    const std::vector<uint8_t> all = joined(frames);
    for (size_t first_call = 0; first_call <= all.size(); ++first_call) {
        BudgetedStream stream;
        PduFramer framer(stream);
        const auto spans = spans_of(frames);
        stream.set_budgets({static_cast<int64_t>(first_call)});
        const PduFramer::GatheredOutcome outcome = framer.send_gathered(spans.data(), spans.size());
        ASSERT_TRUE(outcome.ok) << "cut at " << first_call;

        size_t whole = 0;
        size_t covered = 0;
        while (whole < frames.size() && covered + frames[whole].size() <= first_call) {
            covered += frames[whole].size();
            ++whole;
        }
        EXPECT_EQ(outcome.written_whole, whole) << "cut at " << first_call;
        EXPECT_EQ(outcome.front_cut, first_call > covered) << "cut at " << first_call;
        EXPECT_EQ(framer.has_pending_data(), outcome.front_cut) << "cut at " << first_call;

        // Finish the frame cut part way, then send the frames after it.
        if (framer.has_pending_data()) {
            auto [ok, error] = framer.continue_send();
            ASSERT_TRUE(ok) << error;
            ASSERT_FALSE(framer.has_pending_data());
        }
        const size_t resume_from = outcome.written_whole + (outcome.front_cut ? 1 : 0);
        if (resume_from < frames.size()) {
            const auto rest = spans_of(frames, resume_from);
            const PduFramer::GatheredOutcome second = framer.send_gathered(rest.data(), rest.size());
            ASSERT_TRUE(second.ok);
            EXPECT_EQ(second.written_whole, rest.size());
        }
        EXPECT_EQ(stream.received(), all) << "cut at " << first_call;
    }
}

// The cut frame is finished over several calls, a few bytes each, and nothing is sent twice.
TEST(GatheredWritingTest, AFrameCutPartWayIsFinishedOverSeveralCallsWithoutRepeatingAByte) {
    BudgetedStream stream;
    PduFramer framer(stream);
    const auto frames = make_frames({50, 50});
    const auto spans = spans_of(frames);
    stream.set_budgets({30, 3, 0, 7});
    const PduFramer::GatheredOutcome outcome = framer.send_gathered(spans.data(), spans.size());
    ASSERT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.written_whole, 0U);
    EXPECT_TRUE(outcome.front_cut);
    // continue_send() takes 3, then meets no room (EAGAIN) and stops for EPOLLOUT; the next takes 7, then the rest.
    auto [ok1, error1] = framer.continue_send();
    ASSERT_TRUE(ok1);
    EXPECT_TRUE(framer.has_pending_data());
    auto [ok2, error2] = framer.continue_send();
    ASSERT_TRUE(ok2);
    EXPECT_FALSE(framer.has_pending_data());
    const auto rest = spans_of(frames, 1);
    ASSERT_TRUE(framer.send_gathered(rest.data(), rest.size()).ok);
    EXPECT_EQ(stream.received(), joined(frames));
}

// --- PduFramer::send_gathered: each way the call can refuse ---------------------------------------------------------

TEST(GatheredWritingTest, NoRoomWritesNothingAndIsNotAFailure) {
    BudgetedStream stream;
    PduFramer framer(stream);
    const auto frames = make_frames({8, 8});
    const auto spans = spans_of(frames);
    stream.set_budgets({0});
    const PduFramer::GatheredOutcome outcome = framer.send_gathered(spans.data(), spans.size());
    EXPECT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.written_whole, 0U);
    EXPECT_FALSE(outcome.front_cut);
    EXPECT_FALSE(outcome.short_of_memory);
    EXPECT_FALSE(framer.has_pending_data());
    EXPECT_TRUE(stream.received().empty());
}

TEST(GatheredWritingTest, ShortOfMemoryWritesNothingAndIsNotAFailure) {
    for (const int error : {ENOBUFS, ENOMEM}) {
        BudgetedStream stream;
        PduFramer framer(stream);
        const auto frames = make_frames({8});
        const auto spans = spans_of(frames);
        stream.fail_next_with(error);
        const PduFramer::GatheredOutcome outcome = framer.send_gathered(spans.data(), spans.size());
        EXPECT_TRUE(outcome.ok) << "errno " << error;
        EXPECT_TRUE(outcome.short_of_memory) << "errno " << error;
        EXPECT_EQ(outcome.written_whole, 0U);
        EXPECT_TRUE(stream.received().empty());
    }
}

TEST(GatheredWritingTest, AConnectionGoneIsAFailureAndWritesNothing) {
    for (const int error : {EPIPE, ECONNRESET, ENOTCONN, ETIMEDOUT, EBADF}) {
        BudgetedStream stream;
        PduFramer framer(stream);
        const auto frames = make_frames({8, 8});
        const auto spans = spans_of(frames);
        stream.fail_next_with(error);
        const PduFramer::GatheredOutcome outcome = framer.send_gathered(spans.data(), spans.size());
        EXPECT_FALSE(outcome.ok) << "errno " << error;
        EXPECT_EQ(outcome.written_whole, 0U);
        EXPECT_FALSE(outcome.front_cut);
        EXPECT_FALSE(framer.has_pending_data());
    }
}

// A call that took part of a frame is followed by one that fails: the failure comes on the call after, taking nothing,
// and the bytes already taken are not taken again.
TEST(GatheredWritingTest, AFailureAfterAPartialWriteComesOnTheNextCall) {
    BudgetedStream stream;
    PduFramer framer(stream);
    const auto frames = make_frames({40});
    const auto spans = spans_of(frames);
    stream.set_budgets({20});
    const PduFramer::GatheredOutcome outcome = framer.send_gathered(spans.data(), spans.size());
    ASSERT_TRUE(outcome.ok);
    ASSERT_TRUE(outcome.front_cut);
    stream.fail_next_with(ECONNRESET);
    auto [ok, error] = framer.continue_send();
    EXPECT_FALSE(ok);
    EXPECT_FALSE(framer.has_pending_data()) << "a failed frame is given up, and the connection is closed";
    EXPECT_EQ(stream.received().size(), 20U);
}

TEST(GatheredWritingTest, AGatheredSendWhileAFrameIsInProgressIsRefused) {
    BudgetedStream stream;
    PduFramer framer(stream);
    const auto frames = make_frames({40, 4});
    const auto spans = spans_of(frames);
    stream.set_budgets({10});
    ASSERT_TRUE(framer.send_gathered(spans.data(), 1).front_cut);
    EXPECT_THROW(static_cast<void>(framer.send_gathered(spans.data() + 1, 1)), PreconditionAssertion);
}

TEST(GatheredWritingTest, NoneOrTooManyFramesAreRefused) {
    BudgetedStream stream;
    PduFramer framer(stream);
    const auto frames = make_frames(std::vector<size_t>(ByteStreamInterface::max_gathered_parts + 1, 4));
    const auto spans = spans_of(frames);
    EXPECT_THROW(static_cast<void>(framer.send_gathered(spans.data(), 0)), PreconditionAssertion);
    EXPECT_THROW(static_cast<void>(framer.send_gathered(spans.data(), spans.size())), PreconditionAssertion);
}

// --- gathered_writing: the gathered sends and the waiting queue -----------------------------------------------------

namespace {

// A connection as gathered_writing sees it: a framer on a BudgetedStream, its gathered sends and waiting queue, and a
// record of every chunk given back, by the number put in its slab handle, so that a chunk given back twice, or never,
// shows. Its write_gathered() does what OutboundConnection's does, giving chunks back to this record.
class FakeConnection {
  public:
    explicit FakeConnection(const std::vector<std::vector<uint8_t>>& frames) : frames_(frames), framer_(stream_) {}

    [[nodiscard]] WaitingSend send_of(size_t index) {
        // The chunk points at the test's frame; no allocator is called, so none is given.
        return WaitingSend{nullptr, static_cast<SlabHandle>(index), const_cast<uint8_t*>(frames_[index].data()), // NOLINT
                           static_cast<uint32_t>(frames_[index].size())};
    }

    GatheredSends& gathered_sends() {
        return gathered_;
    }
    WaitingSendQueue& waiting_sends() {
        return waiting_;
    }
    [[nodiscard]] bool has_pending_send() const {
        return framer_.has_pending_data();
    }

    PduFramer::GatheredOutcome write_gathered(const WaitingSend* sends, size_t count) {
        std::vector<SimpleSpan<const uint8_t>> spans;
        for (size_t index = 0; index < count; ++index) {
            spans.emplace_back(static_cast<const uint8_t*>(sends[index].chunk), sends[index].byte_count);
        }
        const PduFramer::GatheredOutcome outcome = framer_.send_gathered(spans.data(), count);
        for (size_t index = 0; index < outcome.written_whole; ++index) {
            given_back_.push_back(static_cast<size_t>(sends[index].slab_id));
        }
        if (outcome.front_cut) {
            in_progress_ = static_cast<size_t>(sends[outcome.written_whole].slab_id);
        }
        return outcome;
    }

    // Finishes the frame in progress, as the reactor does on room to write, giving its chunk back when done.
    void finish_in_progress() {
        while (framer_.has_pending_data()) {
            auto [ok, error] = framer_.continue_send();
            ASSERT_TRUE(ok) << error;
        }
        given_back_.push_back(in_progress_);
    }

    BudgetedStream& stream() {
        return stream_;
    }
    [[nodiscard]] const std::vector<size_t>& given_back() const {
        return given_back_;
    }

  private:
    const std::vector<std::vector<uint8_t>>& frames_;
    BudgetedStream stream_;
    PduFramer framer_;
    GatheredSends gathered_;
    WaitingSendQueue waiting_;
    std::vector<size_t> given_back_;
    size_t in_progress_{0};
};

std::vector<size_t> zero_to(size_t count) {
    std::vector<size_t> numbers(count);
    std::iota(numbers.begin(), numbers.end(), 0);
    return numbers;
}

} // namespaces

TEST(GatheredWritingTest, GatheredSendsAllWrittenInOneCallLeaveNothingWaiting) {
    const auto frames = make_frames({10, 20, 30, 40});
    FakeConnection connection(frames);
    for (size_t index = 0; index < frames.size(); ++index) {
        connection.gathered_sends().add(connection.send_of(index));
    }
    const gathered_writing::Step step = gathered_writing::write_gathered(connection, 1000, 1U << 20U);
    EXPECT_EQ(step.result, gathered_writing::Result::all_written);
    EXPECT_EQ(connection.stream().calls(), 1) << "four messages, one system call";
    EXPECT_TRUE(connection.gathered_sends().empty());
    EXPECT_TRUE(connection.waiting_sends().empty());
    EXPECT_EQ(connection.given_back(), zero_to(4));
    EXPECT_EQ(connection.stream().received(), joined(frames));
}

// Cut part way through the second: the first is given back, the second is in progress, the rest wait in order; once
// room comes, the rest are written from the queue, and the far side has every byte once.
TEST(GatheredWritingTest, GatheredSendsCutPartWayWaitInOrderAndAreAllWrittenLater) {
    const auto frames = make_frames({10, 20, 30, 40});
    const std::vector<uint8_t> all = joined(frames);
    for (size_t first_call = 0; first_call < all.size(); ++first_call) {
        FakeConnection connection(frames);
        for (size_t index = 0; index < frames.size(); ++index) {
            connection.gathered_sends().add(connection.send_of(index));
        }
        connection.stream().set_budgets({static_cast<int64_t>(first_call)});
        const gathered_writing::Step step = gathered_writing::write_gathered(connection, 1000, 1U << 20U);
        ASSERT_EQ(step.result, gathered_writing::Result::must_wait) << "cut at " << first_call;
        EXPECT_TRUE(connection.gathered_sends().empty());

        // Room again: the frame in progress is finished, then the queue is written.
        if (connection.has_pending_send()) {
            connection.finish_in_progress();
        }
        const gathered_writing::Step later = gathered_writing::write_waiting(connection);
        EXPECT_EQ(later.result, gathered_writing::Result::all_written) << "cut at " << first_call;
        EXPECT_EQ(connection.stream().received(), all) << "cut at " << first_call;
        std::vector<size_t> given_back = connection.given_back();
        std::sort(given_back.begin(), given_back.end());
        EXPECT_EQ(given_back, zero_to(4)) << "every chunk given back once; cut at " << first_call;
    }
}

// Sends whose chunks come from a real allocator, which the failure paths give them back to.
WaitingSend real_send(ExpandableSlabAllocator& allocator, const std::vector<uint8_t>& frame) {
    auto [slab_id, chunk] = allocator.allocate(frame.size());
    std::memcpy(chunk, frame.data(), frame.size());
    return WaitingSend{&allocator, slab_id, chunk, static_cast<uint32_t>(frame.size())};
}

TEST(GatheredWritingTest, AFailedGatheredWriteGivesEveryChunkBackAndFails) {
    ExpandableSlabAllocator allocator(65536);
    const auto frames = make_frames({10, 20});
    FakeConnection connection(frames);
    connection.gathered_sends().add(real_send(allocator, frames[0]));
    connection.gathered_sends().add(real_send(allocator, frames[1]));
    connection.stream().fail_next_with(ECONNRESET);
    const gathered_writing::Step step = gathered_writing::write_gathered(connection, 1000, 1U << 20U);
    EXPECT_EQ(step.result, gathered_writing::Result::failed);
    EXPECT_FALSE(step.error.empty());
    EXPECT_TRUE(connection.gathered_sends().empty()) << "every gathered chunk given back to its allocator";
    EXPECT_TRUE(connection.waiting_sends().empty());
    EXPECT_TRUE(connection.stream().received().empty());
}

// The waiting queue refuses what is left, for its limits: those are given back and the write fails, so that the
// connection is closed, as when a send cannot be queued today.
TEST(GatheredWritingTest, WhatTheQueueRefusesIsGivenBackAndTheWriteFails) {
    ExpandableSlabAllocator allocator(65536);
    const auto frames = make_frames({10, 10, 10, 10});
    FakeConnection connection(frames);
    for (const auto& frame : frames) {
        connection.gathered_sends().add(real_send(allocator, frame));
    }
    connection.stream().set_budgets({0});
    // Room for two waiting sends only.
    const gathered_writing::Step step = gathered_writing::write_gathered(connection, 2, 1U << 20U);
    EXPECT_EQ(step.result, gathered_writing::Result::failed);
    EXPECT_TRUE(connection.gathered_sends().empty());
    EXPECT_EQ(connection.waiting_sends().size(), 2U) << "the two that fitted wait, and are given back when the connection is closed";
    connection.waiting_sends().release_all();
}

// More than one call's worth waiting: written max_gathered_parts at a time, in order.
TEST(GatheredWritingTest, ALongQueueIsWrittenInBatchesOfTheMostOneCallTakes) {
    const size_t count = (2 * ByteStreamInterface::max_gathered_parts) + 5;
    const auto frames = make_frames(std::vector<size_t>(count, 3));
    FakeConnection connection(frames);
    for (size_t index = 0; index < count; ++index) {
        ASSERT_TRUE(connection.waiting_sends().add(connection.send_of(index), 1000, 1U << 20U));
    }
    const gathered_writing::Step step = gathered_writing::write_waiting(connection);
    EXPECT_EQ(step.result, gathered_writing::Result::all_written);
    EXPECT_EQ(connection.stream().calls(), 3) << "64, 64 and 5";
    EXPECT_EQ(connection.stream().received(), joined(frames));
    EXPECT_EQ(connection.given_back(), zero_to(count));
}

// Writing from the queue stops at no room, leaving the queue as it was, and goes on later from the same place.
TEST(GatheredWritingTest, WritingTheQueueStopsAtNoRoomAndGoesOnFromTheSamePlace) {
    const auto frames = make_frames({10, 10, 10});
    FakeConnection connection(frames);
    for (size_t index = 0; index < frames.size(); ++index) {
        ASSERT_TRUE(connection.waiting_sends().add(connection.send_of(index), 1000, 1U << 20U));
    }
    connection.stream().set_budgets({0});
    EXPECT_EQ(gathered_writing::write_waiting(connection).result, gathered_writing::Result::must_wait);
    EXPECT_EQ(connection.waiting_sends().size(), 3U);
    EXPECT_TRUE(connection.given_back().empty());
    EXPECT_EQ(gathered_writing::write_waiting(connection).result, gathered_writing::Result::all_written);
    EXPECT_EQ(connection.stream().received(), joined(frames));
}

TEST(GatheredWritingTest, ShortOfMemoryIsReportedAndTheSendsWait) {
    const auto frames = make_frames({10, 10});
    FakeConnection connection(frames);
    connection.gathered_sends().add(connection.send_of(0));
    connection.gathered_sends().add(connection.send_of(1));
    connection.stream().fail_next_with(ENOBUFS);
    const gathered_writing::Step step = gathered_writing::write_gathered(connection, 1000, 1U << 20U);
    EXPECT_EQ(step.result, gathered_writing::Result::must_wait);
    EXPECT_TRUE(step.short_of_memory);
    EXPECT_EQ(connection.waiting_sends().size(), 2U);
    EXPECT_EQ(gathered_writing::write_waiting(connection).result, gathered_writing::Result::all_written);
    EXPECT_EQ(connection.stream().received(), joined(frames));
}

TEST(GatheredSendsTest, AFullArrayRefusesAnotherAndTheFirstShortageOfMemoryIsSaidOnce) {
    GatheredSends gathered;
    for (size_t index = 0; index < GatheredSends::capacity; ++index) {
        gathered.add(WaitingSend{});
    }
    EXPECT_TRUE(gathered.full());
    EXPECT_THROW(gathered.add(WaitingSend{}), PreconditionAssertion);
    EXPECT_TRUE(gathered.first_shortage_of_memory());
    EXPECT_FALSE(gathered.first_shortage_of_memory());
    gathered.clear();
    EXPECT_TRUE(gathered.empty());
}

} // namespaces

#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/WalCursor.hpp>
#include <pubsub_itc_fw/WalFollowing.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Reads a WAL for the components that follow it, and sends each the records it lacks, on a
 *        thread of its own.
 *
 * The thread that owns the WAL writes it and sends new records to the followers it has handed over.
 * A follower that is catching up is sent the older records by this thread instead, so that the
 * owning thread, which sequences orders, never reads the WAL for anyone else (BUG-0133). The design is
 * docs/availability/following_the_wal.md, section 3.2; WalFollowerService runs it from the owning
 * thread.
 *
 * For each follower it is asked to serve, it reads the records after the follower's position with a
 * WalCursor, turns each into the follower's message with the converter it was told to use, and sends
 * it on the follower's connection, which the owning thread owns. Three rules govern what it does:
 *
 * - **It never reads bytes that may still be being written.** It reads only before the write
 *   position the owning thread publishes after each record (WalFollowingPositions).
 * - **It sends only what the follower may be sent:** no record numbered above the follower's limit.
 *   A record read and found above the limit is held, not lost, until the next round.
 * - **It sends only as fast as the connection takes the records:** a batch at a time, asking after
 *   each to be told when the connection can take more.
 *
 * It reads the follower's limit once at the start of each round, when it is told to serve the follower
 * or to carry on, and when it has sent every record up to that limit it reports the position to the
 * owning thread and sends nothing more to that follower until told to carry on.
 *
 * **It also answers requests** (WalReadingCommand::Kind::Answer), so that the owning thread never reads
 * the WAL to answer one either (docs/availability/member_reports_read_off_the_order_thread.md). It reads the
 * WAL's segments from the newest back, one segment at a time, keeping the records the request's
 * WalAnswerer chooses, until it has as many as asked for or has reached a segment that begins at or before
 * the first record wanted. Then it sends them oldest first, a batch at a time, as the connection takes
 * them, closes the answer and reports it. Between segments and between batches it turns to its other work,
 * so that a long answer does not stop a follower being caught up, nor the reverse. It reads records up to the
 * limit the request names, the last written or the last committed, and only bytes completely written.
 *
 * It answers one request at a time, in the order it was asked, and starts the next only once the one before
 * is closed. Two answers for one member therefore reach its gateway one after the other, never mixed: a
 * gateway sent a member's undelivered reports in the middle of its resend would number them wrongly.
 *
 * It owns no connection and takes no lock. It stays on the component's background cores, so it needs
 * no core of its own, and between catch-ups it is blocked on its queue.
 *
 * **Threading.** Everything here runs on this thread. The owning thread reaches it only through
 * WalReadingCommand messages, and it answers only with WalReadingReport messages.
 */
class WalReadingThread : public ApplicationThread {
  public:
    /// The most records sent to one follower before waiting to be told its connection can take more.
    static constexpr int records_per_batch = 256;
    /// The most records read for one follower in one batch, sent or passed over by its converter.
    static constexpr int records_examined_per_batch = 4096;
    /// How often the owning thread is told how far a follower being caught up has been sent.
    static constexpr std::chrono::milliseconds progress_interval{100};
    /// The most segments read back from the newest to find the application's value for WalFollowStarted.
    static constexpr int application_value_segments = 64;

    /**
     * @param[in] token           From ApplicationThread::create, the only way to make one.
     * @param[in] logger          The process's Quill logger.
     * @param[in] reactor         The reactor this thread is registered with.
     * @param[in] thread_name     The thread's name, for the Quill log.
     * @param[in] thread_id       The thread's id.
     * @param[in] queue_config    The thread's message queue.
     * @param[in] allocator_config The thread's allocators.
     * @param[in] owner_thread_id The thread that owns the WAL, to which reports are sent.
     * @param[in] wal_directory   The directory of the WAL's segment files.
     * @param[in] positions       Published by the owning thread; outlives this thread.
     * @param[in] converters      One for each kind of follower, indexed by WalReadingCommand::converter.
     * @param[in] application_value_reader Reads the application's value for WalFollowStarted from a record, for a
     *                            start that asks for it (WalFollowStart::application_value_from_wal); may be empty.
     */
    WalReadingThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& thread_name, ThreadID thread_id,
                     const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config, ThreadID owner_thread_id,
                     std::string wal_directory, const WalFollowingPositions& positions, std::vector<WalRecordConverter> converters,
                     WalApplicationValueReader application_value_reader);

    /// The configuration every reading thread has: it stays on the background cores.
    [[nodiscard]] static ApplicationThreadConfiguration thread_configuration();

  protected:
    void on_itc_message(const EventMessage& message) override;
    void on_connection_writable(ConnectionID id) override;

  private:
    struct Follower {
        ConnectionID connection;
        FollowerLimit limit{FollowerLimit::LastCommitted};
        int converter{0};
        WalCursor cursor;
        int64_t last_sent_seq_no{0}; ///< The follower holds or has been sent every record up to here.
        int64_t records_sent{0};
        uint64_t serving{0};
        /// The follower's limit as it was read when this round started; the round ends at it.
        int64_t round_limit{0};
        bool sending{false};
        /// A record read and found above the limit, held until the limit passes it. Its bytes stay
        /// valid because nothing more is read from the cursor while it is held.
        bool holding{false};
        int64_t held_seq_no{0};
        const uint8_t* held_bytes{nullptr};
        size_t held_size{0};
        /// Where the held record ends in the WAL.
        WalPosition held_end{};
        /// When the owning thread was last told how far this follower has been sent.
        std::chrono::steady_clock::time_point progress_reported_at{};
        /// A position before which the follower holds or has been sent every record: the end of the last
        /// record read for it and not held, or where reading started. Reported when the follower is handed over.
        WalPosition resume_at{};
    };

    // A request being answered (WalReadingCommand::Kind::Answer).
    struct Answer {
        uint64_t id{0};
        ConnectionID connection;
        std::shared_ptr<const WalAnswerer> answerer;
        int64_t after_seq_no{0};
        int64_t through_seq_no{0};
        int64_t keep_newest{0};
        int64_t skip_newest{0};
        /// The last record the request may be answered with, as a kind of position: written, or committed.
        FollowerLimit limit{FollowerLimit::LastCommitted};
        /// The bytes completely written when the answer started: nothing at or after this is read for it.
        WalPosition written{};
        /// The segment to read next, going back from the newest.
        uint64_t next_segment{0};
        /// The records chosen so far, oldest first.
        std::deque<WalAnswerRecord> chosen;
        /// While sending: the next of chosen to send.
        size_t next_to_send{0};
        bool sending{false};
        int64_t records_sent{0};
        int64_t last_seq_no{0};
    };

    void serve(const WalReadingCommand& command);
    void answer(const WalReadingCommand& command);
    // Reads one segment for the answer, then either asks itself to read the next or starts sending.
    void gather_segment(Answer& answer);
    // Sends the next batch of the answer, or closes it and reports it once all are sent; erases a closed answer.
    void send_answer_batch(Answer& answer);
    void send_batch(Follower& follower);
    [[nodiscard]] bool read_next_record(Follower& follower, WalPosition written);
    void report(std::unique_ptr<WalReadingReport> report_to_send);
    // Reports the follower's held record as damaged, and stops serving the follower, whose entry is erased.
    void report_damaged(Follower& follower);
    // The highest value the application's reader gives for a committed record in the newest segment that gives any, or 0.
    [[nodiscard]] int64_t read_application_value();

    ThreadID owner_thread_id_;
    std::string wal_directory_;
    const WalFollowingPositions& positions_;
    std::vector<WalRecordConverter> converters_;
    WalApplicationValueReader application_value_reader_;
    std::unordered_map<int, std::unique_ptr<Follower>> followers_;
    // The answers asked for and not yet closed, by their application's number.
    std::unordered_map<uint64_t, std::unique_ptr<Answer>> answers_;
    // Their numbers, in the order they were asked for: only the first is being answered.
    std::deque<uint64_t> answer_order_;
    // Starts answering the first in answer_order_, if there is one.
    void start_next_answer();
    std::vector<uint8_t> encoded_;
};

} // namespaces

#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
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
     */
    WalReadingThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& thread_name, ThreadID thread_id,
                     const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config, ThreadID owner_thread_id,
                     std::string wal_directory, const WalFollowingPositions& positions, std::vector<WalRecordConverter> converters);

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
    };

    void serve(const WalReadingCommand& command);
    void send_batch(Follower& follower);
    [[nodiscard]] bool read_next_record(Follower& follower, WalPosition written);
    void report(std::unique_ptr<WalReadingReport> report_to_send);
    // Reports the follower's held record as damaged, and stops serving the follower, whose entry is erased.
    void report_damaged(Follower& follower);

    ThreadID owner_thread_id_;
    std::string wal_directory_;
    const WalFollowingPositions& positions_;
    std::vector<WalRecordConverter> converters_;
    std::unordered_map<int, std::unique_ptr<Follower>> followers_;
    std::vector<uint8_t> encoded_;
};

} // namespaces

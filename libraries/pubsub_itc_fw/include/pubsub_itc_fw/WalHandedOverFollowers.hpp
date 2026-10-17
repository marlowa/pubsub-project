#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalCursor.hpp>
#include <pubsub_itc_fw/WalFollowing.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>

namespace pubsub_itc_fw {

/// How WalHandedOverFollowers sends a record. Implemented by the owning thread.
class WalHandedOverFollowersLinksInterface {
  public:
    virtual ~WalHandedOverFollowersLinksInterface() = default;

    /// Send @p follower the @p size bytes at @p bytes as one message of type @p pdu_id, record @p seq_no.
    virtual void send_record(const ConnectionID& follower, int16_t pdu_id, int64_t seq_no, const uint8_t* bytes, size_t size) = 0;
};

/**
 * @brief Sends each follower that has been handed over to new records the records after the last it holds,
 *        read from the WAL by the thread that owns it, as each passes the follower's limit.
 *
 * The design is docs/availability/following_the_wal.md, sections 3.4 and 4.3. A follower such as a
 * matching engine publisher wants every record, as it is committed, after the reading thread has caught it
 * up. Once the follower has been handed over, the thread that owns the WAL sends it those records itself;
 * it no longer holds a record's bytes by the time the record is committed, so it reads them back from the
 * WAL it has just written, which is still in memory. Each follower has its own cursor, started where the
 * reading thread stopped reading for it (WalReadingReport::resume_at), so nothing is searched for.
 *
 * Each record goes through the same converter the reading thread uses for that kind of follower, so the
 * follower is sent the same message whichever thread sends it. A record the converter finds damaged stops
 * that follower being sent anything more: the follower is removed, and the caller is told which record it
 * was, as the reading thread does.
 *
 * **Each record is read once, whatever the number of followers.** Reading a record back checks its checksum,
 * and that check is most of what sending costs, so the followers do not each read the WAL. One reading, the
 * shared reading, reads each record once, converts it once, and sends the result to every follower that is in
 * step with it. A follower is added with a cursor of its own, because it may hold fewer records than the
 * followers already in step, and reads on that cursor until it has every record the shared reading has read;
 * at the next send_to_all it then joins the shared reading and its own cursor is closed. When no follower is
 * left in step, the shared reading is closed too, and the next follower to catch up on its own cursor becomes
 * the shared reading. In normal running a publisher is handed over at the last committed record, which is
 * where the shared reading already is, so it reads on its own for one commit at most.
 *
 * Reads no further than the WAL's write position. Past it lies the unwritten end of the segment, whose zeros
 * the cursor takes for the end of a finished segment, moving on to the next; the cursor would then have to
 * be opened again at its own position, which lists the WAL's directory, at every commit. Opening it again is
 * still what is done when a read fails, so that a segment created since the cursor was opened is found.
 *
 * **Threading.** Everything here runs on the thread that owns the WAL.
 */
class WalHandedOverFollowers {
  public:
    /// What sending to one follower did.
    struct Outcome {
        /// How many records were sent.
        int64_t records_sent{0};
        /// The record the converter found damaged, after which nothing was sent and the follower was removed; 0 if none.
        int64_t damaged_seq_no{0};
    };

    /**
     * @param[in] wal       The WAL, for its write position.
     * @param[in] directory The directory of the WAL's segment files.
     * @param[in] converter Turns a record into the message the followers are sent.
     * @param[in] links     How records are sent.
     */
    WalHandedOverFollowers(const Wal& wal, std::string directory, WalRecordConverter converter, WalHandedOverFollowersLinksInterface& links);

    /**
     * @brief From now on @p follower is sent each record above @p through, read from @p resume_at on.
     *
     * @param[in] follower  The follower's connection. One already present starts afresh.
     * @param[in] through   The last record it holds.
     * @param[in] resume_at A position in the WAL before which it holds every record.
     */
    void add(const ConnectionID& follower, int64_t through, WalPosition resume_at);

    /// @p follower is sent nothing more.
    void remove(const ConnectionID& follower);

    /// No follower is sent anything more.
    void remove_all();

    [[nodiscard]] bool contains(const ConnectionID& follower) const;

    [[nodiscard]] bool empty() const {
        return followers_.empty();
    }

    /// The last record @p follower holds or has been sent; -1 if it is not present.
    [[nodiscard]] int64_t last_sent(const ConnectionID& follower) const;

    /**
     * @brief Sends @p follower every record above the last it holds, up to and including @p through_seq_no,
     *        reading on its own cursor.
     *
     * For a follower that has just been added, to catch it up at once. A follower that has joined the shared
     * reading is sent nothing here, because send_to_all sends to it. Stops at a record the converter finds
     * damaged; the follower is then removed.
     */
    Outcome send_to(const ConnectionID& follower, int64_t through_seq_no);

    /**
     * @brief Sends every follower every record above the last it holds, up to and including @p through_seq_no.
     *
     * Followers still reading on their own cursors are served first, and each that has caught up with the shared
     * reading joins it. Then the shared reading reads each record once and sends it to every follower in step.
     *
     * @return Each follower removed because a record was damaged, with that record's number; usually none.
     */
    std::vector<std::pair<ConnectionID, int64_t>> send_to_all(int64_t through_seq_no);

  private:
    /// A cursor reading the WAL, and the record it has read and is holding until the limit passes it.
    struct Reading {
        WalCursor cursor;
        /// Every record up to here has been read and dealt with: sent, passed over, or found damaged.
        int64_t last_seq_no{0};
        /// Whether a record has been read and found above the limit. Its bytes stay valid because nothing more
        /// is read from the cursor while it is held.
        bool holding{false};
        int64_t held_seq_no{0};
        const uint8_t* held_bytes{nullptr};
        size_t held_size{0};
    };

    struct Follower {
        ConnectionID connection;
        /// The follower holds or has been sent every record up to here.
        int64_t last_sent_seq_no{0};
        /// Its own reading while it catches up with the shared reading; null once it is in step with it.
        std::unique_ptr<Reading> own_reading;
    };

    /// Reads the next record above @p reading's last into its held record; false if nothing more is written.
    [[nodiscard]] bool read_next_record(Reading& reading);

    /// Converts @p reading's held record into encoded_.
    [[nodiscard]] ConvertedRecord convert_held(const Reading& reading);

    /// Sends @p follower the records on its own reading, up to @p through_seq_no.
    Outcome send_on_own_reading(Follower& follower, int64_t through_seq_no);

    /// Sends the followers in step with the shared reading the records up to @p through_seq_no, each read once.
    void send_on_shared_reading(int64_t through_seq_no, std::vector<std::pair<ConnectionID, int64_t>>& damaged);

    /// Closes the shared reading if no follower is in step with it.
    void close_shared_reading_if_unused();

    const Wal& wal_;
    std::string directory_;
    WalRecordConverter converter_;
    WalHandedOverFollowersLinksInterface& links_;
    std::unordered_map<int, std::unique_ptr<Follower>> followers_;
    /// The reading every follower in step with it is sent from; null when no follower is in step.
    std::unique_ptr<Reading> shared_reading_;
    std::vector<uint8_t> encoded_;
    std::vector<ConnectionID> connections_;
};

} // namespaces

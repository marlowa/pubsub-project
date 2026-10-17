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
     * @brief Sends @p follower every record above the last it holds, up to and including @p through_seq_no.
     *
     * Stops at a record the converter finds damaged; the follower is then removed.
     */
    Outcome send_to(const ConnectionID& follower, int64_t through_seq_no);

    /**
     * @brief Sends every follower every record above the last it holds, up to and including @p through_seq_no.
     *
     * @return Each follower removed because a record was damaged, with that record's number; usually none.
     */
    std::vector<std::pair<ConnectionID, int64_t>> send_to_all(int64_t through_seq_no);

  private:
    struct Follower {
        ConnectionID connection;
        WalCursor cursor;
        /// The follower holds or has been sent every record up to here.
        int64_t last_sent_seq_no{0};
        /// A record read and found above the limit, held until the limit passes it. Its bytes stay valid
        /// because nothing more is read from the cursor while it is held.
        bool holding{false};
        int64_t held_seq_no{0};
        const uint8_t* held_bytes{nullptr};
        size_t held_size{0};
    };

    [[nodiscard]] bool read_next_record(Follower& follower);

    const Wal& wal_;
    std::string directory_;
    WalRecordConverter converter_;
    WalHandedOverFollowersLinksInterface& links_;
    std::unordered_map<int, std::unique_ptr<Follower>> followers_;
    std::vector<uint8_t> encoded_;
    std::vector<ConnectionID> connections_;
};

} // namespaces

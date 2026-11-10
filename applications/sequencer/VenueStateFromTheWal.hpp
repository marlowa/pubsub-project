#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <optional>
#include <vector>

#include <pubsub_itc_fw/QuillLogger.hpp>

#include "SessionCancelsPending.hpp"
#include "TradingHaltState.hpp"
#include "VoidedCommandList.hpp"

namespace sequencer {

/**
 * @brief The facts the sequencer reads from its WAL, which every instance keeps whether it leads or follows: whether
 *        trading is halted, which commands are voided, the trading date, and the highest command a report answers
 *        (docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.10).
 *
 * Every record is noted in one way, note_record(), told how the instance came to hold it:
 *
 * | Fact                                   | read_at_open | written_as_follower | written_by_this_leader |
 * |----------------------------------------|--------------|---------------------|------------------------|
 * | whether trading is halted              | yes          | yes                 | yes                    |
 * | the voided commands                    | yes          | yes                 | yes, and the engines are sent the voids once its record is committed |
 * | the highest command a report answers   | yes          | yes                 | yes                    |
 * | the trading date                       | no           | yes, following the leader if they differ | no: a leader reads it from the first record in its WAL
 * (trading_date) |
 *
 * The trading date is the date of the first record of the day, which a leader reads and a follower is told. A record
 * that does not decode is said at Error when the WAL is opened, because a void in it would not be kept.
 *
 * When records are discarded from the WAL, the voids and halts after the point are discarded too. The highest command
 * answered is not lowered: a figure too high only declines a void that could have been written, which costs the rest of
 * the day but never voids a command an engine went past.
 *
 * One thread, the sequencer's, calls every member function.
 */
class VenueStateFromTheWal {
  public:
    /// How the instance came to hold a record.
    enum class How { read_at_open, written_as_follower, written_by_this_leader };

    /// What a record says that this part reads: its command or report, and two fields of its envelope.
    struct Record {
        int16_t pdu_id{0};               ///< the message type of the command or report the envelope carries
        const uint8_t* payload{nullptr}; ///< that command or report, encoded
        size_t size{0};
        bool has_answers_seq_no{false}; ///< for a report: the command it answers
        int64_t answers_seq_no{0};
        bool has_trading_date{false}; ///< for a command: the trading date its leader wrote on it
        int32_t trading_date{0};
        int64_t wall_time_ns{0}; ///< when the record was written, wall-clock nanoseconds since the Unix epoch
    };

    /// What noting a record changed.
    struct Noted {
        bool halt_changed{false}; ///< whether trading is halted changed
        bool void_added{false};   ///< a void not held before
    };

    explicit VenueStateFromTheWal(pubsub_itc_fw::QuillLogger& logger) : logger_(logger) {}

    /// Notes what the record numbered @p seq_no says, held in the way @p how says.
    Noted note_record(int64_t seq_no, const Record& record, How how);

    /// The record numbered @p seq_no did not decode as an envelope; read when the WAL was opened, that is said at Error.
    void record_did_not_decode(int64_t seq_no, How how);

    /// Records after @p seq_no have been discarded from the WAL. @return Whether whether trading is halted changed.
    bool discard_after(int64_t seq_no);

    [[nodiscard]] const TradingHaltState& trading_halt() const {
        return trading_halt_;
    }
    [[nodiscard]] const fix_common::VoidedCommandList& voids() const {
        return voids_;
    }
    /// The cancels on disconnect the WAL says are waiting to be carried out (SessionEnded, SessionEndWithdrawn and
    /// CancelSessionOrders records).
    [[nodiscard]] const SessionCancelsPending& session_cancels() const {
        return session_cancels_;
    }

    /// The highest command any report in the WAL answers.
    [[nodiscard]] int64_t highest_command_answered() const {
        return highest_command_answered_;
    }

    /// Whether the engines are owed the voids, once the record of the latest void written as leader is committed through
    /// @p committed_through; if they are, they are owed them no more.
    [[nodiscard]] bool take_voids_due(int64_t committed_through);

    /// Whether the trading date is known.
    [[nodiscard]] bool trading_date_known() const {
        return trading_date_ != 0;
    }

    /**
     * @brief The trading date, learned now if it is not known: the date of the first record in the WAL, at
     *        @p first_record_wall_time_ns, numbered @p first_seq_no; or, when the WAL holds nothing, the date of the
     *        command being written, at @p wall_time_ns.
     */
    int32_t trading_date(std::optional<int64_t> first_record_wall_time_ns, int64_t first_seq_no, int64_t wall_time_ns);

  private:
    void note_trading_date(int64_t seq_no, const Record& record);
    bool note_void(int64_t seq_no, const Record& record);
    bool note_trading_halt(int64_t seq_no, const Record& record);
    void note_session_cancel(int64_t seq_no, const Record& record);

    pubsub_itc_fw::QuillLogger& logger_;
    TradingHaltState trading_halt_;
    fix_common::VoidedCommandList voids_;
    // The position of a void written as leader and not yet sent to the engines; zero when there is none.
    int64_t void_awaiting_commit_{0};
    // The trading date, as the number yyyymmdd; zero until known.
    int32_t trading_date_{0};
    int64_t highest_command_answered_{0};
    SessionCancelsPending session_cancels_;
    // Scratch space for decoding a void or a TechnicalEvent. A command of the sequencer's own is small, and decoding one
    // needs no more space than this.
    std::vector<uint8_t> decode_arena_ = std::vector<uint8_t>(1024);
};

} // namespaces

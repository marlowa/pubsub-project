#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <vector>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Where the WAL holds the first record answering each of the most recent commands, so that the answer to a
 *        command sent again is read from one known place instead of being searched for (docs/bug_list.md, BUG-0260).
 *
 * The WAL notes, for every record it writes that answers a command, the number of the command's record, the number of
 * the answering record, and where that record starts. Only the first answer to a command is kept.
 *
 * The table has a fixed number of places, a power of two, and the command numbered N is kept in place N modulo that
 * number. So it holds the answers to the most recent commands only: a later command's answer takes the place of an
 * earlier one's. Asked about a command, it says one of three things:
 *
 * | What it says | When |
 * |---|---|
 * | **found**, with the answering record's number and position | the command's place holds that command |
 * | **none yet** | the command is one the table has been noting answers for since it started, and its place holds no later command, so no answer to it has been
 * written | | **not known** | the command came before the table started noting, or its place holds a later command, so its answer, if any, has been displaced |
 *
 * Only "not known" leaves the caller to search the WAL. A command sent again is almost always recent, because it has
 * not been answered, so the search is almost never needed.
 *
 * When the WAL is cut back, the answers recorded may be among the records discarded, so forget_from() forgets them all
 * and the table notes again from there.
 *
 * The storage is allocated once, when the table is created. One thread calls every member function.
 */
class RecentAnswers {
  public:
    /// What the table says about a command.
    enum class Kind { found, none_yet, not_known };

    struct Lookup {
        Kind kind{Kind::not_known};
        /// For found: the number of the first record answering the command.
        int64_t answer_seq_no{0};
        /// For found: where that record starts.
        WalPosition position{};
    };

    /**
     * @param[in] places         How many commands' answers are kept: a power of two, at least 2.
     * @param[in] noting_from    The first command number whose answers the table sees every one of: the number after the
     *                           last record the WAL held when the table was created.
     */
    RecentAnswers(size_t places, int64_t noting_from) : places_(places), mask_(places - 1), noting_from_(noting_from) {
        if (places < 2 || (places & (places - 1)) != 0) {
            throw PreconditionAssertion("RecentAnswers: the number of places must be a power of two, at least 2", __FILE__, __LINE__);
        }
        entries_.resize(places);
    }

    /// The record @p answer_seq_no, which starts at @p position, answers the command in record @p command_seq_no.
    void note(int64_t command_seq_no, int64_t answer_seq_no, WalPosition position) {
        if (command_seq_no <= 0) {
            return;
        }
        Entry& entry = entries_[static_cast<size_t>(command_seq_no) & mask_];
        if (entry.command_seq_no >= command_seq_no) {
            // The first answer to this command is kept already, or a later command holds the place.
            return;
        }
        entry = Entry{command_seq_no, answer_seq_no, position};
    }

    /// What the table says about the command in record @p command_seq_no.
    [[nodiscard]] Lookup lookup(int64_t command_seq_no) const {
        if (command_seq_no < noting_from_) {
            return Lookup{};
        }
        const Entry& entry = entries_[static_cast<size_t>(command_seq_no) & mask_];
        if (entry.command_seq_no == command_seq_no) {
            return Lookup{Kind::found, entry.answer_seq_no, entry.position};
        }
        if (entry.command_seq_no > command_seq_no) {
            return Lookup{};
        }
        return Lookup{Kind::none_yet, 0, {}};
    }

    /// The WAL has been cut back to end before record @p first_discarded: every answer is forgotten, and the table
    /// notes again from there.
    void forget_from(int64_t first_discarded) {
        for (Entry& entry : entries_) {
            entry = Entry{};
        }
        noting_from_ = first_discarded;
    }

    [[nodiscard]] size_t places() const {
        return places_;
    }

  private:
    struct Entry {
        int64_t command_seq_no{0};
        int64_t answer_seq_no{0};
        WalPosition position{};
    };

    const size_t places_;
    const size_t mask_;
    int64_t noting_from_;
    std::vector<Entry> entries_;
};

} // namespaces

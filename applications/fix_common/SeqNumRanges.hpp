#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint> // IWYU pragma: keep
#include <vector>

namespace fix_common {

/**
 * @brief An inclusive run of outbound sequence numbers that carried execution reports.
 *
 * Ranges rather than individual numbers because reports arrive in runs: a member sending
 * orders is sent a contiguous block of numbered reports, broken only when it goes quiet long
 * enough for a heartbeat to take a number. A burst of ten thousand orders is one range.
 */
struct SeqNumRange {
    int32_t from_seq_num{0};
    int32_t to_seq_num{0}; ///< inclusive
};

/**
 * @brief Which outbound sequence numbers carried a message the venue can replay.
 *
 * A resend rewinds a session's outbound number and refills the range from the sequencer's WAL,
 * which holds execution reports and nothing else. Every other message the member was sent -- a
 * Logon, a heartbeat, a reject, a report the gateway synthesised for an order the sequencer
 * never saw -- occupied a number in that range and cannot be produced again. FIX says to gap-fill
 * exactly those, and to do that the venue has to know which they were.
 *
 * These functions maintain that record. A number this covers carried a report and is replayed
 * onto; a number it does not cover is gap-filled, and it does not matter whether that is because
 * nothing replayable was there or because the record no longer reaches back that far. **The two
 * cases want the same treatment, which is why one record answers both** and no separate notion of
 * how far back the venue can vouch for is needed.
 *
 * See docs/availability/resend_provenance.md, and BUG-0051 for what filling every number with a
 * report instead does to a member.
 *
 * Every function here assumes the vector is sorted, non-overlapping and non-adjacent, which is
 * what append() and merge() maintain.
 */
namespace seq_num_ranges {

/// Extends the last range, or starts a new one. Numbers must arrive in increasing order.
inline void append(std::vector<SeqNumRange>& ranges, int32_t seq_num) {
    if (!ranges.empty() && ranges.back().to_seq_num + 1 == seq_num) {
        ranges.back().to_seq_num = seq_num;
        return;
    }
    if (!ranges.empty() && seq_num <= ranges.back().to_seq_num) {
        return; // already covered: a replay re-sending a number it recorded the first time
    }
    ranges.push_back(SeqNumRange{seq_num, seq_num});
}

/// Folds `incoming` into `ranges`, joining runs that meet. Both must be sorted.
inline void merge(std::vector<SeqNumRange>& ranges, const std::vector<SeqNumRange>& incoming) {
    if (incoming.empty()) {
        return;
    }
    ranges.insert(ranges.end(), incoming.begin(), incoming.end());
    std::sort(ranges.begin(), ranges.end(), [](const SeqNumRange& left, const SeqNumRange& right) { return left.from_seq_num < right.from_seq_num; });

    std::vector<SeqNumRange> joined;
    joined.reserve(ranges.size());
    for (const SeqNumRange& range : ranges) {
        if (!joined.empty() && range.from_seq_num <= joined.back().to_seq_num + 1) {
            joined.back().to_seq_num = std::max(joined.back().to_seq_num, range.to_seq_num);
            continue;
        }
        joined.push_back(range);
    }
    ranges.swap(joined);
}

/// True when this number carried a report the venue can replay.
inline bool covers(const std::vector<SeqNumRange>& ranges, int32_t seq_num) {
    const auto next =
        std::upper_bound(ranges.begin(), ranges.end(), seq_num, [](int32_t value, const SeqNumRange& range) { return value < range.from_seq_num; });
    return next != ranges.begin() && seq_num <= std::prev(next)->to_seq_num;
}

/// How many covered numbers lie in [from, to] inclusive.
inline int32_t count_in(const std::vector<SeqNumRange>& ranges, int32_t from_seq_num, int32_t to_seq_num) {
    int32_t total = 0;
    for (const SeqNumRange& range : ranges) {
        if (range.from_seq_num > to_seq_num) {
            break;
        }
        const int32_t overlap_from = std::max(range.from_seq_num, from_seq_num);
        const int32_t overlap_to = std::min(range.to_seq_num, to_seq_num);
        if (overlap_from <= overlap_to) {
            total += overlap_to - overlap_from + 1;
        }
    }
    return total;
}

/// The ranges from `floor` upward, for shipping only what the far side has not been told.
inline std::vector<SeqNumRange> from_seq_num(const std::vector<SeqNumRange>& ranges, int32_t floor_seq_num) {
    std::vector<SeqNumRange> tail;
    for (const SeqNumRange& range : ranges) {
        if (range.to_seq_num < floor_seq_num) {
            continue;
        }
        tail.push_back(SeqNumRange{std::max(range.from_seq_num, floor_seq_num), range.to_seq_num});
    }
    return tail;
}

/**
 * @brief Drops what falls below the retention window, keeping at most `keep` numbers covered.
 *
 * What is dropped becomes uncovered, which is the same state as a number that never carried a
 * report: the venue gap-fills it. So retention degrades a resend's reach without ever making it
 * wrong, and there is no second mechanism to keep in step with this one.
 */
inline void trim(std::vector<SeqNumRange>& ranges, int32_t keep) {
    if (ranges.empty() || keep <= 0) {
        return;
    }
    const int32_t highest = ranges.back().to_seq_num;
    const int32_t floor_seq_num = highest - keep + 1;
    if (floor_seq_num <= ranges.front().from_seq_num) {
        return;
    }
    std::vector<SeqNumRange> kept = from_seq_num(ranges, floor_seq_num);
    ranges.swap(kept);
}

/**
 * @brief How many numbers a session remembers as having carried a report.
 *
 * Bounded so the record cannot grow for the life of a session -- the shape of BUG-0048, which
 * this must not repeat. The value is a constant rather than configuration until the WAL's own
 * retention is settled: the two have to agree, because provenance for numbers whose reports the
 * WAL no longer holds buys nothing, and a venue that believed it could serve a range it cannot
 * would gap-fill after promising otherwise. See docs/availability/resend_provenance.md.
 */
inline constexpr int32_t max_remembered = 100000;

} // namespaces

} // namespaces

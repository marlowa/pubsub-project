#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

namespace sequencer {

/**
 * @brief Where an execution report stands in the sequence of reports the matching engine sends.
 *
 * The leading matching engine numbers its reports from 1 within each leadership, and sends the epoch
 * of that leadership with each number. A new engine leadership has a higher epoch and numbers from 1
 * again, so positions are ordered by epoch first and number second, and that order is the order the
 * engines sent the reports in.
 *
 * The leading sequencer uses positions to say how far it has forwarded the engine's reports, and a
 * follower uses that to discard the copies of reports it keeps in case it takes the lead
 * (docs/bug_list.md, BUG-0116).
 */
struct EngineReportPosition {
    int32_t epoch{0};
    int64_t number{0};

    [[nodiscard]] bool is_before_or_at(const EngineReportPosition& other) const {
        return epoch < other.epoch || (epoch == other.epoch && number <= other.number);
    }

    [[nodiscard]] bool is_after(const EngineReportPosition& other) const {
        return !is_before_or_at(other);
    }

    /// The position just before this one in the same leadership: everything before this report.
    [[nodiscard]] EngineReportPosition just_before() const {
        return EngineReportPosition{epoch, number - 1};
    }
};

} // namespaces

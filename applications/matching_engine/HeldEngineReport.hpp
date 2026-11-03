#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <vector>

#include "SessionIdentity.hpp"

namespace matching_engine {

/**
 * @brief A report from the matching engine that is not sent at once: held by EngineReportSending until it can be sent,
 *        or kept, while the engine follows the WAL, until the leader's report of the same command has been compared
 *        with it.
 *
 * The encoded bytes are held rather than the ExecutionReport, whose string fields point into the decode arena and do
 * not outlive the record that produced them.
 */
struct HeldEngineReport {
    /// The encoded ExecutionReport.
    std::vector<uint8_t> payload;
    /// The number of the command the report answers, sent in the transport header, as a live report's is.
    int64_t seq_no{0};
    /// The member's session the report is for.
    fix_common::SessionIdentity session;
    /// Whether the report is marked as a possible repeat.
    bool poss_resend{false};
    /// Whether the report has been given its number in the engine's reports. It keeps that number, so that both
    /// sequencers see the same number for it.
    bool numbered{false};
    int32_t report_engine_epoch{0};
    int64_t report_number{0};
    /// Which connections for reports it has been sent on; a held report is kept until it has been sent on each.
    bool sent_to_primary{false};
    bool sent_to_secondary{false};
    /// For a report kept while following: whether the leader's report of the same command has been compared with it.
    bool compared{false};
};

} // namespaces

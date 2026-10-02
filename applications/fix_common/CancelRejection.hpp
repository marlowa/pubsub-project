#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <fix_orders.hpp>

namespace fix_common {

/**
 * @brief Whether a matching engine report is the engine refusing a request to cancel.
 *
 * The engine answers a request to cancel an order it does not hold with a rejected
 * ExecutionReport carrying the OrigClOrdID of the order the request named. A rejected new order
 * never carries an OrigClOrdID, so the two cannot be confused. Both gateways send such a report
 * to the member as an OrderCancelReject, not as a rejected ExecutionReport, which says that an
 * order was rejected (R-0151, docs/bug_list.md BUG-0099).
 *
 * @param[in] report The matching engine's report.
 * @return True if the report refuses a request to cancel.
 */
[[nodiscard]] inline bool is_cancel_rejection(const pubsub_itc_fw_app::ExecutionReportView& report) {
    return report.exec_type == pubsub_itc_fw_app::ExecType::Rejected && report.has_orig_cl_ord_id && !report.orig_cl_ord_id.empty();
}

/**
 * @brief The CxlRejReason an OrderCancelReject carries for a matching engine's refusal of a cancel.
 *
 * The engine refuses a cancel only when it holds no such order, which FIX calls Unknown order.
 * Any other reason it might give is reported as Other, with the engine's text alongside.
 *
 * @param[in] report A report for which is_cancel_rejection is true.
 * @return UnknownOrder or Other.
 */
[[nodiscard]] inline pubsub_itc_fw_app::CxlRejReason cancel_reject_reason_for(const pubsub_itc_fw_app::ExecutionReportView& report) {
    const bool unknown_order = report.has_ord_rej_reason && report.ord_rej_reason == pubsub_itc_fw_app::OrdRejReason::UnknownOrder;
    return unknown_order ? pubsub_itc_fw_app::CxlRejReason::UnknownOrder : pubsub_itc_fw_app::CxlRejReason::Other;
}

} // namespaces

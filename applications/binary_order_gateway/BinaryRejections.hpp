#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string_view>

#include <binary_session.hpp>
#include <fix_orders.hpp>

#include "RequestRefusal.hpp"

namespace binary_order_gateway {

/**
 * @brief An ExecutionReport rejecting a new order that the gateway, or the sequencer, refused without passing it to the
 *        matching engine.
 *
 * The report echoes the order's ClOrdID, Symbol, Side and OrderQty, says that nothing was filled and nothing is left,
 * and carries @p ord_rej_reason and @p reason. The views it holds point at the arguments, which must outlive it.
 *
 * @param[in] order_id The OrderID the gateway made for the rejection, since the matching engine never saw the order.
 * @param[in] exec_id The ExecID the gateway made for the rejection.
 * @param[in] order The refused order.
 * @param[in] reason The text to send.
 * @param[in] ord_rej_reason The reason code to send.
 * @param[in] transact_time_ns When the gateway refused the order, in nanoseconds since the Unix epoch.
 */
[[nodiscard]] inline pubsub_itc_fw_app::ExecutionReport make_reject_report(std::string_view order_id, std::string_view exec_id,
                                                                           const pubsub_itc_fw_app::NewOrderSingleView& order, std::string_view reason,
                                                                           pubsub_itc_fw_app::OrdRejReason ord_rej_reason, int64_t transact_time_ns) {
    pubsub_itc_fw_app::ExecutionReport report{};
    report.order_id = order_id;
    report.exec_id = exec_id;
    report.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
    report.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
    report.symbol = order.symbol;
    report.side = order.side;
    report.leaves_qty = "0";
    report.cum_qty = "0";
    report.avg_px = "0";
    report.transact_time = transact_time_ns;
    report.has_cl_ord_id = true;
    report.cl_ord_id = order.cl_ord_id;
    report.has_order_qty = true;
    report.order_qty = order.order_qty;
    report.has_ord_rej_reason = true;
    report.ord_rej_reason = ord_rej_reason;
    report.has_text = true;
    report.text = reason;
    return report;
}

/**
 * @brief The RequestRefused message answering a request to cancel that the gateway, or the sequencer, refused before it
 *        reached the matching engine.
 *
 * It refuses the request and says nothing of the order the request names: no OrderID and no order status, since the
 * component that holds the order never saw the request (R-0151, docs/venue/gateway_refusals.md). The views it holds
 * point at the arguments, which must outlive it.
 *
 * @param[in] request The refused request.
 * @param[in] reason The kind of refusal, the same number the FIX gateway gives in SessionRejectReason (373).
 * @param[in] text The reason in words.
 * @param[in] field The field at fault, as the binary protocol names it; empty when no one field is at fault.
 */
[[nodiscard]] inline pubsub_itc_fw_app::RequestRefused make_cancel_refused(const pubsub_itc_fw_app::OrderCancelRequestView& request,
                                                                           fix_common::RefusalReason reason, std::string_view text, std::string_view field) {
    pubsub_itc_fw_app::RequestRefused refused{};
    refused.cl_ord_id = request.cl_ord_id;
    refused.refused_pdu_id = pubsub_itc_fw_app::OrderCancelRequest::message_pdu_id;
    refused.reason = static_cast<pubsub_itc_fw_app::RefusalReason>(static_cast<int32_t>(reason));
    refused.text = text;
    if (!field.empty()) {
        refused.has_field = true;
        refused.field = field;
    }
    return refused;
}

// The binary protocol's reasons are the shared ones, number for number, so that converting one to the other is a cast.
static_assert(static_cast<int32_t>(pubsub_itc_fw_app::RefusalReason::RequiredFieldMissing) ==
              static_cast<int32_t>(fix_common::RefusalReason::RequiredFieldMissing));
static_assert(static_cast<int32_t>(pubsub_itc_fw_app::RefusalReason::ValueIncorrect) == static_cast<int32_t>(fix_common::RefusalReason::ValueIncorrect));
static_assert(static_cast<int32_t>(pubsub_itc_fw_app::RefusalReason::Other) == static_cast<int32_t>(fix_common::RefusalReason::Other));
static_assert(static_cast<int32_t>(pubsub_itc_fw_app::RefusalReason::ThrottleLimitExceeded) ==
              static_cast<int32_t>(fix_common::RefusalReason::ThrottleLimitExceeded));
static_assert(static_cast<int32_t>(pubsub_itc_fw_app::RefusalReason::VenueUnavailable) == static_cast<int32_t>(fix_common::RefusalReason::VenueUnavailable));

} // namespaces

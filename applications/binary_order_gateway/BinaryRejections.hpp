#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string_view>

#include <fix_orders.hpp>

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
 * @brief An OrderCancelReject refusing a request to cancel that the gateway, or the sequencer, refused without passing
 *        it to the matching engine.
 *
 * The reply says the order is still open (OrdStatus New), because the refusal has not touched it. The gateway's record
 * of a member's open orders is not the book, so the gateway never claims an order has gone. The views it holds point at
 * the arguments, which must outlive it.
 *
 * @param[in] order_id The OrderID of the order the request names, or "NONE" when the gateway does not know it.
 * @param[in] request The refused request.
 * @param[in] reason The text to send.
 * @param[in] cxl_rej_reason The reason code to send.
 * @param[in] transact_time_ns When the gateway refused the request, in nanoseconds since the Unix epoch.
 */
[[nodiscard]] inline pubsub_itc_fw_app::OrderCancelReject make_cancel_reject(std::string_view order_id,
                                                                             const pubsub_itc_fw_app::OrderCancelRequestView& request, std::string_view reason,
                                                                             pubsub_itc_fw_app::CxlRejReason cxl_rej_reason, int64_t transact_time_ns) {
    pubsub_itc_fw_app::OrderCancelReject reject{};
    reject.order_id = order_id;
    reject.cl_ord_id = request.cl_ord_id;
    reject.has_orig_cl_ord_id = true;
    reject.orig_cl_ord_id = request.orig_cl_ord_id;
    reject.ord_status = pubsub_itc_fw_app::OrdStatus::New;
    reject.has_transact_time = true;
    reject.transact_time = transact_time_ns;
    reject.cxl_rej_response_to = pubsub_itc_fw_app::CxlRejResponseTo::OrderCancelRequest;
    reject.has_cxl_rej_reason = true;
    reject.cxl_rej_reason = cxl_rej_reason;
    reject.has_text = true;
    reject.text = reason;
    return reject;
}

} // namespaces

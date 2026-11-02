#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string_view>

#include <fix_orders.hpp>

#include "FixMessage.hpp"

namespace fix_order_gateway {

/**
 * @brief An ExecutionReport (35=8) rejecting a new order that the gateway, or the sequencer, refused without passing it
 *        to the matching engine.
 *
 * Tag values follow FIX 5.0SP2: 150 = 8 and 39 = 8 (Rejected); 11, 55, 54 and 38 echoed from the order; 151 = 0 and
 * 14 = 0 when the quantity is echoed; 103 = @p ord_rej_reason; 58 = @p reason. Side and OrderQty are optional on a
 * rejection, since the matching engine never priced the order, but echoing what the member sent matches the format of
 * a real report for the same order.
 *
 * @param[in] order_id The OrderID the gateway made for the rejection, since the matching engine never saw the order.
 * @param[in] exec_id The ExecID the gateway made for the rejection.
 * @param[in] cl_ord_id The order's ClOrdID.
 * @param[in] symbol The order's Symbol.
 * @param[in] side_str The order's Side as its FIX value; empty to leave tag 54 out.
 * @param[in] order_qty The order's OrderQty; empty to leave tags 38, 151 and 14 out.
 * @param[in] reason The text for tag 58.
 * @param[in] ord_rej_reason The value for tag 103.
 * @return The message, with every value copied into it.
 */
[[nodiscard]] FixMessage make_reject_execution_report(std::string_view order_id, std::string_view exec_id, std::string_view cl_ord_id, std::string_view symbol,
                                                      std::string_view side_str, std::string_view order_qty, std::string_view reason,
                                                      pubsub_itc_fw_app::OrdRejReason ord_rej_reason);

/**
 * @brief An OrderCancelReject (35=9) refusing a request to cancel that the gateway, or the sequencer, refused without
 *        passing it to the matching engine.
 *
 * Tag values follow FIX 5.0SP2: 37 = @p order_id; 11 and 41 echoed from the request; 39 = 0 (New: the order the request
 * names is still open, because the refusal has not touched it); 434 = 1 (a request to cancel); 102 = @p cxl_rej_reason;
 * 58 = @p reason.
 *
 * @param[in] order_id The OrderID of the order the request names, or "NONE" when the gateway does not know it.
 * @param[in] cl_ord_id The request's ClOrdID.
 * @param[in] orig_cl_ord_id The OrigClOrdID of the order the request names; empty to leave tag 41 out.
 * @param[in] reason The text for tag 58.
 * @param[in] cxl_rej_reason The value for tag 102.
 * @return The message, with every value copied into it.
 */
[[nodiscard]] FixMessage make_order_cancel_reject(std::string_view order_id, std::string_view cl_ord_id, std::string_view orig_cl_ord_id,
                                                  std::string_view reason, pubsub_itc_fw_app::CxlRejReason cxl_rej_reason);

} // namespaces

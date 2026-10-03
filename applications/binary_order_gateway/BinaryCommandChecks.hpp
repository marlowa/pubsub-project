#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <fix_codec/FixField.hpp>
#include <fix_orders.hpp>

#include "FixOrderLimits.hpp"

namespace binary_order_gateway {

/**
 * @file BinaryCommandChecks.hpp
 * @brief The checks the binary gateway makes on a member's new order or cancel before passing it on.
 *
 * They are the checks the FIX gateway makes on the same commands, expressed for the binary
 * protocol, so that a member is refused the same commands whichever protocol it speaks:
 *
 * - **Every field holds a value its definition allows.** An enumerated field (Side, OrdType,
 *   TimeInForce) holds a value the enumeration defines, and a required string is not empty. This
 *   is the generated first_invalid_field, made from the same definition as the decoder. The FIX
 *   gateway gets the same from the FIX dictionary validator.
 * - **Identifiers are no longer than the matching engine's book key holds**,
 *   fix_order_limits::max_cl_ord_id_length, which is shared with the FIX gateway.
 * - **Symbol and OrderQty are no longer than configured**, which is what the open-order record can
 *   hold. The FIX gateway has the same two limits in its own configuration.
 * - **Every quantity and price is a decimal number**, by the FIX codec's own rule
 *   (fix_codec::FixField::as_decimal), since the binary protocol carries them as text as FIX does.
 *
 * Each function returns an empty view when the command passes, and otherwise the reason, written
 * into the caller's buffer. The reason names the field as the binary protocol names it. The checks
 * that depend on the state of the venue -- a sequencer connected, the venue accepting orders, the
 * session's throttle -- are the gateway's, and are made after these.
 *
 * A command whose ClOrdID is empty cannot be refused, because the refusal must name it. The caller
 * checks for that before calling.
 */

/** @brief The configured limits on the length of a command's fields. */
struct CommandFieldLimits {
    size_t max_symbol_length{0};
    size_t max_order_qty_length{0};
};

namespace detail {

/// True if @p text is a decimal number as FIX defines one: an optional sign, digits, and an
/// optional decimal point with digits.
[[nodiscard]] inline bool is_decimal(std::string_view text) {
    int64_t mantissa = 0;
    int exponent = 0;
    return fix_codec::FixField{0, text}.as_decimal(mantissa, exponent);
}

/// Writes a reason into @p buffer and returns it, cut short rather than overrunning.
template <typename... Arguments>
[[nodiscard]] std::string_view write_reason(char* buffer, size_t capacity, fmt::format_string<Arguments...> format, Arguments&&... arguments) {
    const auto written = fmt::format_to_n(buffer, capacity, format, std::forward<Arguments>(arguments)...);
    return std::string_view(buffer, written.size < capacity ? written.size : capacity);
}

[[nodiscard]] inline std::string_view check_length(std::string_view name, std::string_view value, size_t limit, char* buffer, size_t capacity) {
    if (value.size() <= limit) {
        return {};
    }
    return write_reason(buffer, capacity, "{} exceeds maximum length of {}", name, limit);
}

[[nodiscard]] inline std::string_view check_decimal(std::string_view name, std::string_view value, char* buffer, size_t capacity) {
    if (is_decimal(value)) {
        return {};
    }
    return write_reason(buffer, capacity, "{} is not a decimal number", name);
}

} // namespaces

/// Room for any reason these functions write.
inline constexpr size_t command_refusal_text_capacity = 128;

/**
 * @brief Checks a member's new order.
 * @param[in]  order    The decoded order. Its ClOrdID is not empty.
 * @param[in]  limits   The configured field lengths.
 * @param[out] buffer   Where a reason is written.
 * @param[in]  capacity The size of @p buffer.
 * @return An empty view if the order passes, otherwise the reason it is refused.
 */
[[nodiscard]] inline std::string_view check_new_order(const pubsub_itc_fw_app::NewOrderSingleView& order, const CommandFieldLimits& limits, char* buffer,
                                                      size_t capacity) {
    const std::string_view invalid = pubsub_itc_fw_app::first_invalid_field(order);
    if (!invalid.empty()) {
        return detail::write_reason(buffer, capacity, "{} is missing or holds a value the protocol does not define", invalid);
    }
    std::string_view reason = detail::check_length("cl_ord_id", order.cl_ord_id, fix_order_limits::max_cl_ord_id_length, buffer, capacity);
    if (reason.empty()) {
        reason = detail::check_length("symbol", order.symbol, limits.max_symbol_length, buffer, capacity);
    }
    if (reason.empty()) {
        reason = detail::check_length("order_qty", order.order_qty, limits.max_order_qty_length, buffer, capacity);
    }
    if (reason.empty()) {
        reason = detail::check_decimal("order_qty", order.order_qty, buffer, capacity);
    }
    if (reason.empty() && order.has_price) {
        reason = detail::check_decimal("price", order.price, buffer, capacity);
    }
    if (reason.empty() && order.has_stop_px) {
        reason = detail::check_decimal("stop_px", order.stop_px, buffer, capacity);
    }
    if (reason.empty() && order.has_min_qty) {
        reason = detail::check_decimal("min_qty", order.min_qty, buffer, capacity);
    }
    if (reason.empty() && order.has_max_floor) {
        reason = detail::check_decimal("max_floor", order.max_floor, buffer, capacity);
    }
    for (const auto& underlying : order.no_underlyings) {
        if (!reason.empty()) {
            break;
        }
        if (underlying.has_underlying_qty) {
            reason = detail::check_decimal("underlying_qty", underlying.underlying_qty, buffer, capacity);
        }
    }
    return reason;
}

/**
 * @brief Checks a member's request to cancel an order.
 * @param[in]  request  The decoded request. Its ClOrdID is not empty.
 * @param[in]  limits   The configured field lengths.
 * @param[out] buffer   Where a reason is written.
 * @param[in]  capacity The size of @p buffer.
 * @return An empty view if the request passes, otherwise the reason it is refused.
 */
[[nodiscard]] inline std::string_view check_cancel(const pubsub_itc_fw_app::OrderCancelRequestView& request, const CommandFieldLimits& limits, char* buffer,
                                                   size_t capacity) {
    const std::string_view invalid = pubsub_itc_fw_app::first_invalid_field(request);
    if (!invalid.empty()) {
        return detail::write_reason(buffer, capacity, "{} is missing or holds a value the protocol does not define", invalid);
    }
    std::string_view reason = detail::check_length("cl_ord_id", request.cl_ord_id, fix_order_limits::max_cl_ord_id_length, buffer, capacity);
    if (reason.empty()) {
        reason = detail::check_length("orig_cl_ord_id", request.orig_cl_ord_id, fix_order_limits::max_cl_ord_id_length, buffer, capacity);
    }
    if (reason.empty()) {
        reason = detail::check_length("symbol", request.symbol, limits.max_symbol_length, buffer, capacity);
    }
    // OrderQty is optional on a cancel (R-0142); when it is given, it is held to the same rules as
    // an order's.
    if (reason.empty() && request.has_order_qty) {
        reason = detail::check_length("order_qty", request.order_qty, limits.max_order_qty_length, buffer, capacity);
        if (reason.empty()) {
            reason = detail::check_decimal("order_qty", request.order_qty, buffer, capacity);
        }
    }
    return reason;
}

} // namespaces

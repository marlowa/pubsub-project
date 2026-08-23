#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string_view>

#include <fix_orders.hpp>

namespace open_orders {

/**
 * @brief Which resting order, if any, an ExecutionReport retires.
 *
 * A gateway keeps the orders a session has resting, keyed by the ClOrdID the
 * member gave when it sent them. Deciding which of those an incoming report
 * closes out is not as simple as reading its ClOrdID, and reading it was a bug
 * that leaked every cancelled order for the life of a session.
 *
 * A report that retires an order because the member asked for it to be
 * cancelled names **two** orders. ClOrdID is the cancel request -- an
 * identifier the member minted for the request itself, which was never a
 * resting order and was never filed. OrigClOrdID is the order actually being
 * retired, and that is the one on file. Looking up ClOrdID therefore searches
 * for something that has never been in the map, finds nothing, and removes
 * nothing, while the entry and its pooled record stay for good.
 *
 * A report that retires an order on the venue's own initiative -- a fill, an
 * expiry, a rejection of the order itself -- carries only ClOrdID, and that
 * does name the resting order. So the rule is to prefer OrigClOrdID and fall
 * back to ClOrdID, not to swap one for the other.
 *
 * ## A rejected cancel retires nothing
 *
 * A rejection carrying OrigClOrdID is a cancel that did **not** happen: the
 * engine is saying it could not act on the request. Both possible mistakes are
 * available here and they do not cost the same. An entry wrongly kept costs
 * memory until the session ends, and cancel-on-disconnect issues one cancel for
 * an order already gone, which the engine rejects. An entry wrongly removed
 * means a live order resting on the book that the gateway no longer knows
 * about, so when the session drops it is not cancelled -- it simply stays there
 * with nobody managing it, which is the outcome cancel-on-disconnect exists to
 * prevent. The cheap error is to keep, so this keeps.
 */
/**
 * @brief Whether an OrdStatus retires an order, so it stops resting on the book.
 *
 * Both gateways held their own identical copy of this. One copy, because the two must agree:
 * a status one treats as terminal and the other does not means the same order is dropped by
 * one protocol's members and kept by the other's.
 */
inline bool is_terminal_ord_status(pubsub_itc_fw_app::OrdStatus status) {
    switch (status) {
        case pubsub_itc_fw_app::OrdStatus::Filled:
        case pubsub_itc_fw_app::OrdStatus::Canceled:
        case pubsub_itc_fw_app::OrdStatus::DoneForDay:
        case pubsub_itc_fw_app::OrdStatus::Rejected:
        case pubsub_itc_fw_app::OrdStatus::Expired:
            return true;
        default:
            return false;
    }
}

struct OpenOrderRemoval {
    /// True when a resting order should be removed and its pooled record released.
    bool remove{false};

    /// The key it is filed under. Meaningful only when remove is true.
    std::string_view key{};
};

/**
 * @brief Decide what an ExecutionReport removes from a session's open orders.
 * @param[in] ord_status          The report's OrdStatus.
 * @param[in] has_cl_ord_id       Whether ClOrdID is present.
 * @param[in] cl_ord_id           ClOrdID: the request this report answers.
 * @param[in] has_orig_cl_ord_id  Whether OrigClOrdID is present.
 * @param[in] orig_cl_ord_id      OrigClOrdID: the order a cancel names.
 *
 * Deliberately takes the fields rather than the report, so it can be tested
 * without building a PDU and so both gateways can call it from the different
 * shapes they decode into.
 */
inline OpenOrderRemoval decide_open_order_removal(pubsub_itc_fw_app::OrdStatus ord_status, bool has_cl_ord_id, std::string_view cl_ord_id,
                                                  bool has_orig_cl_ord_id, std::string_view orig_cl_ord_id) {
    if (!is_terminal_ord_status(ord_status)) {
        return {};
    }

    const bool names_another_order = has_orig_cl_ord_id && !orig_cl_ord_id.empty();

    if (names_another_order) {
        if (ord_status == pubsub_itc_fw_app::OrdStatus::Rejected) {
            // The cancel was refused, so the order it named is still resting.
            return {};
        }
        return {true, orig_cl_ord_id};
    }

    if (!has_cl_ord_id || cl_ord_id.empty()) {
        // Nothing names anything. Removing on a guess would unfile a live order.
        return {};
    }
    return {true, cl_ord_id};
}

} // namespaces

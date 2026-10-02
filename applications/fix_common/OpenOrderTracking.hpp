#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstring>
#include <string_view>

#include <fix_orders.hpp>
#include <pubsub_itc_fw/ExpandablePoolAllocator.hpp>

#include "FixOrderLimits.hpp"
#include "OpenOrderEntry.hpp"
#include "OpenOrderRemoval.hpp"

namespace open_orders {

/** @brief What track_open_order did with a report. */
enum class TrackOutcome {
    /// The report names no order (it has no ClOrdID), so nothing was done.
    NoOrderNamed,
    /// A terminal report retired an order the session had on file.
    Removed,
    /// A terminal report retired nothing: the order was not on file, or the report was a refused cancel.
    NothingToRemove,
    /// A new order was put on file.
    Added,
    /// An order already on file was updated in place, for example by a partial fill.
    Updated,
    /// A field was too long for the entry's fixed storage, so the order was not put on file.
    FieldTooLong
};

/**
 * @brief Keeps a session's record of its open orders up to date from one matching engine report.
 *
 * Both gateways keep, for each session, the orders it has resting on the book, so that
 * cancel-on-disconnect can cancel them and a refused cancel can name them. This is the one copy of
 * the rule both use, because two copies drifted apart: one guarded the entry's fixed storage
 * against over-long fields and updated a repeated report in place, and the other did neither.
 *
 * - **A terminal report** (the order filled, cancelled or rejected) retires the order it names,
 *   by the rule in open_orders::decide_open_order_removal, and returns its entry to the pool.
 * - **A non-terminal report** puts the order on file. If the order is already on file, its entry is
 *   updated in place: allocating a second entry would leave the first in the pool for good, because
 *   the map holds one entry for each ClOrdID.
 * - **A field too long** for the entry's fixed arrays is refused, and nothing is filed. The gateway
 *   checks ClOrdID where the member's message arrives, and the symbol and quantity come from the
 *   matching engine, so this guards against a defect upstream rather than member input. The caller
 *   logs it, in its own words.
 *
 * Only reports from the matching engine reach this: the record holds orders the engine has
 * accepted, not orders a member has sent.
 *
 * @param[in,out] orders  The session's open orders.
 * @param[in,out] pool    Where entries are allocated from and returned to.
 * @param[in]     report  The matching engine's report. Its string_views must stay valid for the call.
 * @return What was done.
 */
inline TrackOutcome track_open_order(OpenOrderMap& orders, pubsub_itc_fw::ExpandablePoolAllocator<OpenOrderEntry>& pool,
                                     const pubsub_itc_fw_app::ExecutionReportView& report) {
    if (!report.has_cl_ord_id) {
        return TrackOutcome::NoOrderNamed;
    }

    if (is_terminal_ord_status(report.ord_status)) {
        const OpenOrderRemoval removal =
            decide_open_order_removal(report.ord_status, report.has_cl_ord_id, report.cl_ord_id, report.has_orig_cl_ord_id, report.orig_cl_ord_id);
        if (!removal.remove) {
            return TrackOutcome::NothingToRemove;
        }
        // Looking up by string_view compares contents, so this finds the entry keyed by the pool
        // storage without building a std::string.
        const auto existing = orders.find(removal.key);
        if (existing == orders.end()) {
            return TrackOutcome::NothingToRemove;
        }
        pool.deallocate(existing->second);
        orders.erase(existing);
        return TrackOutcome::Removed;
    }

    const size_t cl_ord_id_length = report.cl_ord_id.size();
    const size_t symbol_length = report.symbol.size();
    const size_t order_qty_length = report.has_order_qty ? report.order_qty.size() : 0;
    if (cl_ord_id_length > fix_order_limits::max_cl_ord_id_length || symbol_length > max_supported_symbol_length ||
        order_qty_length > max_supported_order_qty_length) {
        return TrackOutcome::FieldTooLong;
    }

    const auto tracked = orders.find(report.cl_ord_id);
    const bool already_tracked = tracked != orders.end();
    OpenOrderEntry* entry = already_tracked ? tracked->second : pool.allocate();

    std::memcpy(entry->cl_ord_id, report.cl_ord_id.data(), cl_ord_id_length);
    entry->cl_ord_id[cl_ord_id_length] = '\0';
    entry->cl_ord_id_len = static_cast<uint8_t>(cl_ord_id_length);
    std::memcpy(entry->symbol, report.symbol.data(), symbol_length);
    entry->symbol[symbol_length] = '\0';
    entry->symbol_len = static_cast<uint8_t>(symbol_length);
    if (order_qty_length > 0) {
        std::memcpy(entry->order_qty, report.order_qty.data(), order_qty_length);
    }
    entry->order_qty[order_qty_length] = '\0';
    entry->order_qty_len = static_cast<uint8_t>(order_qty_length);
    entry->side = static_cast<char>(report.side);
    // Kept so cancel-on-disconnect can leave persistent orders resting. Absent means the member sent
    // no TimeInForce, which implies Day and claims no exemption, so zero rather than an enum value.
    entry->time_in_force = report.has_time_in_force ? static_cast<char>(report.time_in_force) : char{0};
    // Kept so a refused request to cancel can name the order on its reply.
    set_order_id(*entry, report.order_id);

    if (already_tracked) {
        return TrackOutcome::Updated;
    }
    // The key views the pool storage, which is stable for the entry's lifetime.
    orders.emplace(std::string_view(entry->cl_ord_id, entry->cl_ord_id_len), entry);
    return TrackOutcome::Added;
}

} // namespaces

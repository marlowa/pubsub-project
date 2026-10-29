// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "OrderBook.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace matching_engine {

OrderBook::~OrderBook() = default;

OrderBook::OrderBook(pubsub_itc_fw::AllocationGrowthReporter* reporter) : index_(0, OrderKeyHash{}, std::equal_to<OrderKey>{}, reporter) {}

bool OrderBook::open(const std::string& region_path, SlotIndex region_capacity, size_t map_capacity) {
    if (region_path.empty()) {
        throw pubsub_itc_fw::PreconditionAssertion("OrderBook: a book needs a region path", __FILE__, __LINE__);
    }

    const bool existed = store_.open(region_path, static_cast<uint32_t>(sizeof(OrderEntry)), region_capacity, record_format);
    halted_ = store_.owner_value() != 0;
    index_.reserve(map_capacity);
    return existed;
}

const OrderEntry* OrderBook::entry_at(SlotIndex slot) const {
    return reinterpret_cast<const OrderEntry*>(store_.payload(slot));
}

OrderEntry* OrderBook::entry_at(SlotIndex slot) {
    return reinterpret_cast<OrderEntry*>(store_.payload(slot));
}

bool OrderBook::contains(const OrderKey& key) const {
    return index_.count(key) != 0;
}

const OrderEntry* OrderBook::find(const OrderKey& key) const {
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return nullptr;
    }
    return entry_at(it->second);
}

bool OrderBook::add(const OrderKey& key, const OrderEntry& entry, int64_t seq_no) {
    const SlotIndex slot = store_.acquire();
    if (slot == pubsub_itc_fw::MappedSlotStore::no_slot) {
        return false;
    }

    // The record first, then the stamp that makes it live, then the map. A death between them
    // leaves a record above the published position, which recovery ignores.
    OrderEntry* written = entry_at(slot);
    std::memcpy(written, &entry, sizeof(OrderEntry));
    // Written here rather than left to the caller. The identity is what files the record, so a
    // record that did not carry it could be stored and then never found again.
    written->session = key.session;
    written->set_cl_ord_id(std::string_view(key.cl_ord_id.data(), key.cl_ord_id_len));
    store_.commit(slot, seq_no);
    // The map can fail to grow. The record is released again, so that a failure here leaves the book as it was rather
    // than holding a record the map does not know of, which a restart would then put on the book.
    try {
        index_.emplace(key, slot);
    } catch (const std::exception&) {
        store_.release(slot);
        throw;
    }
    return true;
}

bool OrderBook::remove(const OrderKey& key) {
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    const SlotIndex slot = it->second;
    index_.erase(it);
    store_.release(slot);
    return true;
}

OrderBook::Recovery OrderBook::recover() {
    Recovery found;
    found.published = store_.published();

    const SlotIndex capacity = store_.capacity();
    for (SlotIndex slot = 0; slot < capacity; ++slot) {
        if (!store_.is_recoverable(slot)) {
            // Either free, or written by work that had not finished when the process died.
            // The sequencer's record still holds that work, and its tail will run it again.
            if (store_.is_live(slot)) {
                ++found.discarded;
            }
            continue;
        }
        const OrderEntry* entry = entry_at(slot);
        const OrderKey key = OrderKey::make(entry->session, entry->get_cl_ord_id());
        index_.emplace(key, slot);
        ++found.orders;
    }

    // Never trusted, always re-derived. The free list is changed on every accept and every
    // cancel, so a process that died mid-change can leave it holding a dangling index or a
    // cycle; the scan above has already paid for knowing which records are in use.
    store_.rebuild_free_list();
    return found;
}

std::string OrderBook::consistency_problem() const {
    size_t live = 0;
    const SlotIndex capacity = store_.capacity();
    for (SlotIndex slot = 0; slot < capacity; ++slot) {
        if (store_.is_live(slot)) {
            ++live;
        }
    }
    if (live != index_.size()) {
        return fmt::format("the region holds {} record(s) in use and the map {} order(s)", live, index_.size());
    }
    for (const auto& kv : index_) {
        if (!store_.is_live(kv.second)) {
            return fmt::format("the map files an order under record {}, which is not in use", kv.second);
        }
        const OrderEntry* entry = entry_at(kv.second);
        if (!(OrderKey::make(entry->session, entry->get_cl_ord_id()) == kv.first)) {
            return fmt::format("record {} holds an order other than the one the map files under it", kv.second);
        }
        // Walking the map visits every entry it holds, but a lookup can still fail to reach one, which is the failure a
        // member sees: a cancel of an order the venue holds refused as unknown.
        if (index_.find(kv.first) == index_.end()) {
            return fmt::format("the map holds the order in record {} but a lookup by its key does not find it", kv.second);
        }
    }
    return {};
}

void OrderBook::clear() {
    for (const auto& kv : index_) {
        store_.release(kv.second);
    }
    index_.clear();
}

} // namespaces

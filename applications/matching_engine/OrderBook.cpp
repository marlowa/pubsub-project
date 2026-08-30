// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "OrderBook.hpp"

#include <cstring>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace matching_engine {

OrderBook::~OrderBook() = default;

OrderBook::OrderBook(pubsub_itc_fw::AllocationGrowthReporter* reporter) : index_(0, OrderKeyHash{}, std::equal_to<OrderKey>{}, reporter) {}

bool OrderBook::open(const std::string& region_path, SlotIndex region_capacity, size_t map_capacity) {
    if (region_path.empty()) {
        throw pubsub_itc_fw::PreconditionAssertion("OrderBook: a book needs a region path", __FILE__, __LINE__);
    }

    const bool existed = store_.open(region_path, static_cast<uint32_t>(sizeof(OrderEntry)), region_capacity);
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
    std::memcpy(entry_at(slot), &entry, sizeof(OrderEntry));
    store_.commit(slot, seq_no);
    index_.emplace(key, slot);
    return true;
}

bool OrderBook::add_or_replace(const OrderKey& key, const OrderEntry& entry, int64_t seq_no) {
    if (SlotIndex* existing = index_.find_value(key)) {
        std::memcpy(entry_at(*existing), &entry, sizeof(OrderEntry));
        store_.commit(*existing, seq_no);
        return true;
    }
    return add(key, entry, seq_no);
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

void OrderBook::clear() {
    for (const auto& kv : index_) {
        store_.release(kv.second);
    }
    index_.clear();
}

} // namespaces

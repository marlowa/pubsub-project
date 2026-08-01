#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>

#include <fix_codec/FixField.hpp>
#include <fix_codec/FixGroupWalker.hpp>
#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/fix_dictionary.hpp>

#include <pubsub_itc_fw/BumpAllocator.hpp>

// The generated PDU structs (NewOrderSingle, Underlyings, PartyIDs, PartySubIDs and
// ListView). Included, like FixErEncoder.hpp, after authentication.hpp has defined
// BytesView -- so this header must be included after FixOrderGatewayThread.hpp.
#include <fix_orders.hpp>

namespace fix_order_gateway {

/**
 * @brief Populate the repeating groups of a validated NewOrderSingle into its PDU.
 *
 * The gateway's flat ParsedFixMessage cannot represent repeating groups, so the
 * group-carrying NOS fields (NoUnderlyings, NoPartyIDs and nested NoPartySubIDs) are
 * read straight from the framed FIX bytes. All the FIX group mechanics -- instance
 * boundaries, nested descent, consuming groups the demo PDU does not model -- live in
 * fix_codec::walk_repeating_group; this file supplies only the application-specific
 * mapping from FIX tags into the generated structs (a small visitor per group). A
 * second FIX gateway would reuse the same walker with its own visitors.
 *
 * The message has already passed FixMessageValidator, so the structure is known
 * well-formed: every NUMINGROUP counter equals the instances present and each instance
 * begins with its delimiter tag.
 *
 * Lifetimes:
 *   - Element arrays are allocated from the arena, which must outlive the subsequent
 *     encode of the NewOrderSingle (the gateway forwards synchronously).
 *   - Captured string_views point into the FixMessageReader's byte buffer, valid for
 *     the duration of the parser callback -- again spanning the synchronous forward.
 */
namespace fix_group_tag {
// FIX tag numbers for the NewOrderSingle repeating groups. The gateway Tag enum only
// covers the flat fields it handles directly, so the group members are named here.
constexpr int no_underlyings = 711;
constexpr int underlying_symbol = 311;
constexpr int underlying_security_id = 309;
constexpr int underlying_qty = 879;

constexpr int no_party_ids = 453;
constexpr int party_id = 448;
constexpr int party_id_source = 447;
constexpr int party_role = 452;
constexpr int no_party_sub_ids = 802;
constexpr int party_sub_id = 523;
constexpr int party_sub_id_type = 803;
} // namespaces

namespace fix_group_extractor_detail {

/**
 * @brief Visitor mapping the NoUnderlyings group into ListView<Underlyings>.
 *
 * The demo Underlyings struct models no nested list, so nested groups the client may
 * send under an underlying (e.g. NoUnderlyingSecurityAltID) are skipped by the walker.
 */
struct UnderlyingsVisitor {
    pubsub_itc_fw_app::Underlyings* elements;
    size_t capacity;
    size_t count = 0;
    pubsub_itc_fw_app::Underlyings* current = nullptr;

    void enter_instance(int /*depth*/, const fix_codec::group_def& /*def*/, size_t index) {
        current = index < capacity ? &elements[index] : nullptr;
        if (current != nullptr) {
            *current = pubsub_itc_fw_app::Underlyings{};
            count = index + 1;
        }
    }
    bool field(int /*depth*/, const fix_codec::group_def& /*def*/, size_t /*index*/, int /*member*/, const fix_codec::FixField& value) {
        if (current == nullptr) {
            return true;
        }
        switch (value.tag) {
            case fix_group_tag::underlying_symbol:
                current->has_underlying_symbol = true;
                current->underlying_symbol = value.value;
                break;
            case fix_group_tag::underlying_security_id:
                current->has_underlying_security_id = true;
                current->underlying_security_id = value.value;
                break;
            case fix_group_tag::underlying_qty:
                current->has_underlying_qty = true;
                current->underlying_qty = value.value;
                break;
            default:
                break;
        }
        return true;
    }
    bool leave_instance(int /*depth*/, const fix_codec::group_def& /*def*/, size_t /*index*/) {
        return true;
    }
    bool orphan_field(int /*depth*/, const fix_codec::group_def& /*def*/, const fix_codec::FixField& /*value*/) {
        return true;
    }
    bool enter_nested(int /*depth*/, const fix_codec::group_def& /*def*/, size_t /*index*/, const fix_codec::FixField& /*counter*/,
                      const fix_codec::group_def& /*nested*/) {
        return false; // Underlyings models no nested list -> let the walker skip it
    }
    bool leave_group(int /*depth*/, const fix_codec::group_def& /*def*/, size_t /*instances*/, int /*declared*/) {
        return true;
    }
};

/**
 * @brief Visitor mapping the NoPartyIDs group (and its nested NoPartySubIDs) into
 *        ListView<PartyIDs>. depth 0 is a PartyIDs instance; depth 1 a PartySubIDs.
 */
struct PartyIDsVisitor {
    pubsub_itc_fw::BumpAllocator& arena;
    pubsub_itc_fw_app::PartyIDs* elements;
    size_t capacity;
    size_t count = 0;
    pubsub_itc_fw_app::PartyIDs* current = nullptr;

    pubsub_itc_fw_app::PartySubIDs* sub_elements = nullptr;
    size_t sub_capacity = 0;
    size_t sub_count = 0;
    pubsub_itc_fw_app::PartySubIDs* current_sub = nullptr;

    void enter_instance(int depth, const fix_codec::group_def& /*def*/, size_t index) {
        if (depth == 0) {
            current = index < capacity ? &elements[index] : nullptr;
            if (current != nullptr) {
                *current = pubsub_itc_fw_app::PartyIDs{};
                count = index + 1;
            }
        } else {
            current_sub = index < sub_capacity ? &sub_elements[index] : nullptr;
            if (current_sub != nullptr) {
                *current_sub = pubsub_itc_fw_app::PartySubIDs{};
                sub_count = index + 1;
            }
        }
    }
    bool field(int depth, const fix_codec::group_def& /*def*/, size_t /*index*/, int /*member*/, const fix_codec::FixField& value) {
        if (depth == 0) {
            if (current == nullptr) {
                return true;
            }
            switch (value.tag) {
                case fix_group_tag::party_id:
                    current->has_party_id = true;
                    current->party_id = value.value;
                    break;
                case fix_group_tag::party_id_source:
                    current->has_party_id_source = true;
                    current->party_id_source = static_cast<pubsub_itc_fw_app::PartyIDSource>(value.as_char(0));
                    break;
                case fix_group_tag::party_role:
                    current->has_party_role = true;
                    current->party_role = static_cast<pubsub_itc_fw_app::PartyRole>(value.as_int(0));
                    break;
                default:
                    break;
            }
        } else {
            if (current_sub == nullptr) {
                return true;
            }
            switch (value.tag) {
                case fix_group_tag::party_sub_id:
                    current_sub->has_party_sub_id = true;
                    current_sub->party_sub_id = value.value;
                    break;
                case fix_group_tag::party_sub_id_type:
                    current_sub->has_party_sub_id_type = true;
                    current_sub->party_sub_id_type = static_cast<pubsub_itc_fw_app::PartySubIDType>(value.as_int(0));
                    break;
                default:
                    break;
            }
        }
        return true;
    }
    bool leave_instance(int /*depth*/, const fix_codec::group_def& /*def*/, size_t /*index*/) {
        return true;
    }
    bool orphan_field(int /*depth*/, const fix_codec::group_def& /*def*/, const fix_codec::FixField& /*value*/) {
        return true;
    }
    bool enter_nested(int depth, const fix_codec::group_def& /*def*/, size_t /*index*/, const fix_codec::FixField& counter,
                      const fix_codec::group_def& /*nested*/) {
        // Only the PartyIDs level has a modeled nested list (NoPartySubIDs).
        if (depth != 0 || current == nullptr || counter.tag != fix_group_tag::no_party_sub_ids) {
            return false;
        }
        const int declared = fix_codec::parse_num_in_group(counter.value);
        if (declared <= 0) {
            return false;
        }
        sub_elements = arena.allocate<pubsub_itc_fw_app::PartySubIDs>(static_cast<size_t>(declared));
        if (sub_elements == nullptr) {
            return false; // arena exhausted: skip the nested group rather than emit a partial one
        }
        sub_capacity = static_cast<size_t>(declared);
        sub_count = 0;
        current_sub = nullptr;
        current->no_party_sub_i_ds.data = sub_elements; // size filled in when the nested group closes
        return true;
    }
    bool leave_group(int depth, const fix_codec::group_def& /*def*/, size_t /*instances*/, int /*declared*/) {
        if (depth == 1 && current != nullptr) {
            current->no_party_sub_i_ds.size = sub_count;
        }
        return true;
    }
};

/** @brief Fill @p out with the NoUnderlyings group beginning after its counter. */
inline void extract_underlyings(int declared, fix_codec::group_iterator& it, const fix_codec::group_iterator& end, pubsub_itc_fw::BumpAllocator& arena,
                                pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::Underlyings>& out) {
    const int group = fix_codec::group_index_for_counter(fix_group_tag::no_underlyings);
    if (group < 0) {
        return;
    }
    const fix_codec::group_def& def = fix_codec::group_at(group);
    if (declared <= 0) {
        fix_codec::skip_repeating_group(def, it, end);
        return;
    }
    auto* elements = arena.allocate<pubsub_itc_fw_app::Underlyings>(static_cast<size_t>(declared));
    if (elements == nullptr) {
        fix_codec::skip_repeating_group(def, it, end);
        return;
    }
    UnderlyingsVisitor visitor{elements, static_cast<size_t>(declared)};
    fix_codec::walk_repeating_group(def, declared, it, end, 0, visitor);
    out.data = elements;
    out.size = visitor.count;
}

/** @brief Fill @p out with the NoPartyIDs group (and nested NoPartySubIDs) after its counter. */
inline void extract_party_ids(int declared, fix_codec::group_iterator& it, const fix_codec::group_iterator& end, pubsub_itc_fw::BumpAllocator& arena,
                              pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::PartyIDs>& out) {
    const int group = fix_codec::group_index_for_counter(fix_group_tag::no_party_ids);
    if (group < 0) {
        return;
    }
    const fix_codec::group_def& def = fix_codec::group_at(group);
    if (declared <= 0) {
        fix_codec::skip_repeating_group(def, it, end);
        return;
    }
    auto* elements = arena.allocate<pubsub_itc_fw_app::PartyIDs>(static_cast<size_t>(declared));
    if (elements == nullptr) {
        fix_codec::skip_repeating_group(def, it, end);
        return;
    }
    PartyIDsVisitor visitor{arena, elements, static_cast<size_t>(declared)};
    fix_codec::walk_repeating_group(def, declared, it, end, 0, visitor);
    out.data = elements;
    out.size = visitor.count;
}

} // namespaces

/**
 * @brief Populate the repeating-group fields of @p nos from the framed FIX message.
 *
 * Walks the reader's top-level field stream and, on each NoUnderlyings/NoPartyIDs
 * counter, descends to fill the corresponding ListView. See the file header for the
 * arena and string_view lifetime requirements.
 */
inline void extract_new_order_single_groups(const fix_codec::FixMessageReader& reader, pubsub_itc_fw::BumpAllocator& arena,
                                            pubsub_itc_fw_app::NewOrderSingle& nos) {
    fix_codec::group_iterator it = reader.begin();
    const fix_codec::group_iterator end = reader.end();
    while (it != end) {
        const fix_codec::FixField field = *it;
        const int tag = field.tag;
        const int declared = field.as_int(0);
        if (tag == fix_group_tag::no_underlyings) {
            ++it;
            fix_group_extractor_detail::extract_underlyings(declared, it, end, arena, nos.no_underlyings);
            continue; // the extractor left `it` beyond the group
        }
        if (tag == fix_group_tag::no_party_ids) {
            ++it;
            fix_group_extractor_detail::extract_party_ids(declared, it, end, arena, nos.no_party_i_ds);
            continue;
        }
        ++it;
    }
}

} // namespaces

#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <string_view>

#include <fix_codec/FixField.hpp>
#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/fix_dictionary.hpp>

#include <pubsub_itc_fw/BumpAllocator.hpp>

// The generated PDU structs (NewOrderSingle, Underlyings, PartyIDs, PartySubIDs and
// ListView). Included, like FixErEncoder.hpp, after authentication.hpp has defined
// BytesView -- so this header must be included after OrderGatewayThread.hpp.
#include <fix_equity_orders.hpp>

namespace order_gateway {

/**
 * @brief Extracts the repeating groups of a validated NewOrderSingle into its
 *        generated PDU struct.
 *
 * The gateway's flat ParsedFixMessage cannot represent repeating groups, so the
 * group-carrying fields of a NewOrderSingle (NoUnderlyings, NoPartyIDs and the
 * nested NoPartySubIDs) are extracted straight from the framed FIX bytes via the
 * FixMessageReader token stream. The walk is driven by the same global group table
 * (fix_codec::group_def) that FixMessageValidator uses, so nested groups the demo
 * PDU does not model are still consumed correctly rather than ending a group early.
 *
 * The message has already passed FixMessageValidator by the time this runs, so the
 * structure is known well-formed: every NUMINGROUP counter equals the number of
 * instances present and each instance begins with its delimiter tag.
 *
 * Lifetimes:
 *   - Element arrays are allocated from @p arena. It must outlive the subsequent
 *     encode of the NewOrderSingle (the gateway forwards synchronously, so a
 *     call-scoped arena suffices).
 *   - Captured string_views point into the FixMessageReader's byte buffer, valid
 *     for the duration of the parser callback -- again, spanning the synchronous
 *     forward.
 */
namespace fix_group_tag {
// FIX tag numbers for the NewOrderSingle repeating groups. The gateway Tag enum
// only covers the flat fields it handles directly, so the group members are named
// here rather than left as bare literals in the walk below.
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

using const_iterator = fix_codec::FixMessageReader::const_iterator;

/** @brief Index of @p tag within a group body, or -1 if it is not a member. */
inline int group_member_index(const fix_codec::group_def& def, int tag) {
    for (size_t member = 0; member < def.member_count; ++member) {
        if (def.members[member].tag == tag) {
            return static_cast<int>(member);
        }
    }
    return -1;
}

/**
 * @brief Advance @p it past one repeating group's instances without capturing.
 *
 * On entry @p it points at the first field after the NUMINGROUP counter; on return
 * at the first field that is not a member of @p def. Nested groups are consumed
 * recursively so their members do not leak into the enclosing scope. Used to skip
 * groups the demo PDU does not model (e.g. the nested groups under NoUnderlyings).
 */
inline void skip_group(const fix_codec::group_def& def, const_iterator& it, const const_iterator& end) {
    while (it != end) {
        const fix_codec::FixField& field = *it;
        const int member = group_member_index(def, field.tag);
        if (member < 0) {
            return; // not a member of this group -> the group has ended
        }
        const int nested = def.members[static_cast<size_t>(member)].nested_group;
        ++it;
        if (nested >= 0) {
            skip_group(fix_codec::group_at(nested), it, end);
        }
    }
}

/** @brief Extract the NoPartySubIDs instances nested within one PartyIDs instance. */
inline void extract_party_sub_ids(const fix_codec::group_def& def, int declared, const_iterator& it, const const_iterator& end,
                                  pubsub_itc_fw::BumpAllocator& arena, pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::PartySubIDs>& out) {
    if (declared <= 0) {
        return;
    }
    auto* elements = arena.allocate<pubsub_itc_fw_app::PartySubIDs>(static_cast<size_t>(declared));
    if (elements == nullptr) {
        skip_group(def, it, end); // arena exhausted: still consume the group so the walk resumes correctly
        return;
    }

    size_t count = 0;
    pubsub_itc_fw_app::PartySubIDs* current = nullptr;
    while (it != end) {
        // Copy, not reference: *it aliases the iterator's internal FixField, which ++it
        // overwrites. The captured tag/value (the value_view points into the stable message
        // buffer, not the iterator) must stay valid across the advance below.
        const fix_codec::FixField field = *it;
        const int member = group_member_index(def, field.tag);
        if (member < 0) {
            break; // group ended
        }
        if (field.tag == def.delimiter_tag && count < static_cast<size_t>(declared)) {
            current = &elements[count];
            *current = pubsub_itc_fw_app::PartySubIDs{};
            ++count;
        }
        const int nested = def.members[static_cast<size_t>(member)].nested_group;
        ++it;
        if (nested >= 0) {
            skip_group(fix_codec::group_at(nested), it, end);
            continue;
        }
        if (current == nullptr) {
            continue;
        }
        switch (field.tag) {
            case fix_group_tag::party_sub_id:
                current->has_party_sub_id = true;
                current->party_sub_id = field.value;
                break;
            case fix_group_tag::party_sub_id_type:
                current->has_party_sub_id_type = true;
                current->party_sub_id_type = static_cast<pubsub_itc_fw_app::PartySubIDType>(field.as_int(0));
                break;
            default:
                break;
        }
    }
    out.data = elements;
    out.size = count;
}

/** @brief Extract the NoPartyIDs instances (and their nested NoPartySubIDs) of a NewOrderSingle. */
inline void extract_party_ids(int declared, const_iterator& it, const const_iterator& end, pubsub_itc_fw::BumpAllocator& arena,
                              pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::PartyIDs>& out) {
    const int group = fix_codec::group_index_for_counter(fix_group_tag::no_party_ids);
    if (group < 0) {
        return;
    }
    const fix_codec::group_def& def = fix_codec::group_at(group);
    if (declared <= 0) {
        skip_group(def, it, end);
        return;
    }
    auto* elements = arena.allocate<pubsub_itc_fw_app::PartyIDs>(static_cast<size_t>(declared));
    if (elements == nullptr) {
        skip_group(def, it, end);
        return;
    }

    size_t count = 0;
    pubsub_itc_fw_app::PartyIDs* current = nullptr;
    while (it != end) {
        // Copy, not reference: *it aliases the iterator's internal FixField, which ++it
        // overwrites (see extract_party_sub_ids).
        const fix_codec::FixField field = *it;
        const int member = group_member_index(def, field.tag);
        if (member < 0) {
            break; // group ended
        }
        if (field.tag == def.delimiter_tag && count < static_cast<size_t>(declared)) {
            current = &elements[count];
            *current = pubsub_itc_fw_app::PartyIDs{};
            ++count;
        }
        const int nested = def.members[static_cast<size_t>(member)].nested_group;
        ++it;
        if (nested >= 0) {
            if (field.tag == fix_group_tag::no_party_sub_ids && current != nullptr) {
                extract_party_sub_ids(fix_codec::group_at(nested), field.as_int(0), it, end, arena, current->no_party_sub_i_ds);
            } else {
                skip_group(fix_codec::group_at(nested), it, end);
            }
            continue;
        }
        if (current == nullptr) {
            continue;
        }
        switch (field.tag) {
            case fix_group_tag::party_id:
                current->has_party_id = true;
                current->party_id = field.value;
                break;
            case fix_group_tag::party_id_source:
                current->has_party_id_source = true;
                current->party_id_source = static_cast<pubsub_itc_fw_app::PartyIDSource>(field.as_char(0));
                break;
            case fix_group_tag::party_role:
                current->has_party_role = true;
                current->party_role = static_cast<pubsub_itc_fw_app::PartyRole>(field.as_int(0));
                break;
            default:
                break;
        }
    }
    out.data = elements;
    out.size = count;
}

/** @brief Extract the NoUnderlyings instances of a NewOrderSingle. */
inline void extract_underlyings(int declared, const_iterator& it, const const_iterator& end, pubsub_itc_fw::BumpAllocator& arena,
                                pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::Underlyings>& out) {
    const int group = fix_codec::group_index_for_counter(fix_group_tag::no_underlyings);
    if (group < 0) {
        return;
    }
    const fix_codec::group_def& def = fix_codec::group_at(group);
    if (declared <= 0) {
        skip_group(def, it, end);
        return;
    }
    auto* elements = arena.allocate<pubsub_itc_fw_app::Underlyings>(static_cast<size_t>(declared));
    if (elements == nullptr) {
        skip_group(def, it, end);
        return;
    }

    size_t count = 0;
    pubsub_itc_fw_app::Underlyings* current = nullptr;
    while (it != end) {
        // Copy, not reference: *it aliases the iterator's internal FixField, which ++it
        // overwrites (see extract_party_sub_ids).
        const fix_codec::FixField field = *it;
        const int member = group_member_index(def, field.tag);
        if (member < 0) {
            break; // group ended
        }
        if (field.tag == def.delimiter_tag && count < static_cast<size_t>(declared)) {
            current = &elements[count];
            *current = pubsub_itc_fw_app::Underlyings{};
            ++count;
        }
        const int nested = def.members[static_cast<size_t>(member)].nested_group;
        ++it;
        if (nested >= 0) {
            skip_group(fix_codec::group_at(nested), it, end); // Underlyings models no nested list
            continue;
        }
        if (current == nullptr) {
            continue;
        }
        switch (field.tag) {
            case fix_group_tag::underlying_symbol:
                current->has_underlying_symbol = true;
                current->underlying_symbol = field.value;
                break;
            case fix_group_tag::underlying_security_id:
                current->has_underlying_security_id = true;
                current->underlying_security_id = field.value;
                break;
            case fix_group_tag::underlying_qty:
                current->has_underlying_qty = true;
                current->underlying_qty = field.value;
                break;
            default:
                break;
        }
    }
    out.data = elements;
    out.size = count;
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
    using namespace fix_group_extractor_detail;
    const_iterator it = reader.begin();
    const const_iterator end = reader.end();
    while (it != end) {
        const fix_codec::FixField& field = *it;
        const int tag = field.tag;
        const int declared = field.as_int(0);
        if (tag == fix_group_tag::no_underlyings) {
            ++it;
            extract_underlyings(declared, it, end, arena, nos.no_underlyings);
            continue; // the extractor left `it` beyond the group
        }
        if (tag == fix_group_tag::no_party_ids) {
            ++it;
            extract_party_ids(declared, it, end, arena, nos.no_party_i_ds);
            continue;
        }
        ++it;
    }
}

} // namespaces

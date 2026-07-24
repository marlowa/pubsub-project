#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <string_view>

#include <fix_codec/FixField.hpp>
#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/fix_dictionary.hpp>

namespace fix_codec {

/**
 * @brief Generic FIX repeating-group traversal, driven by the dictionary group_def.
 *
 * This is the one place that understands the mechanics of a FIX repeating group:
 * instances begin with the group's delimiter tag, a member whose nested_group is set
 * opens a sub-group, and the group ends at the first field that is not a member. Both
 * the dictionary-driven validator and the application-side group extractors are built
 * on it, so the descent logic -- and its subtleties, like copying each FixField before
 * advancing the iterator -- lives once. A new FIX consumer (e.g. a second gateway)
 * reuses this to read groups and supplies only its own per-field handling.
 */

/// Guards against a pathological (cyclic) dictionary; real FIX nesting is only a few
/// levels deep. Beyond this a nested counter is treated as a plain field, bounding
/// stack use.
inline constexpr int max_group_depth = 16;

using group_iterator = FixMessageReader::const_iterator;

/** @brief Index of @p tag within a group body, or -1 if it is not a member. */
inline int group_member_index(const group_def& def, int tag) {
    for (size_t member = 0; member < def.member_count; ++member) {
        if (def.members[member].tag == tag) {
            return static_cast<int>(member);
        }
    }
    return -1;
}

/** @brief Non-negative NUMINGROUP count from a (format-validated) integer value. */
inline int parse_num_in_group(std::string_view text) {
    int value = 0;
    for (const char character : text) {
        if (character >= '0' && character <= '9') { // skip a leading sign; the field was already format-checked
            value = value * 10 + (character - '0');
        }
    }
    return value;
}

/**
 * @brief Advance @p it past one repeating group's instances without visiting them.
 *
 * On entry @p it points at the first field after the NUMINGROUP counter; on return at
 * the first field that is not a member of @p def. Nested groups are consumed
 * recursively so their members do not leak into the enclosing scope. Used to consume a
 * group a consumer does not model.
 */
inline void skip_repeating_group(const group_def& def, group_iterator& it, const group_iterator& end) {
    while (it != end) {
        const FixField field = *it; // copy: *it aliases iterator-owned storage that ++it overwrites
        const int member = group_member_index(def, field.tag);
        if (member < 0) {
            return; // not a member of this group -> the group has ended
        }
        const int nested = def.members[static_cast<size_t>(member)].nested_group;
        ++it;
        if (nested >= 0) {
            skip_repeating_group(group_at(nested), it, end);
        }
    }
}

/**
 * @brief Drive the standard repeating-group walk over [@p it, @p end) using @p def.
 *
 * Detects instance boundaries by the delimiter tag, copies each field before advancing
 * (so the visitor may read it safely), recurses into nested groups the visitor accepts
 * and skips the rest, and stops at the first non-member field. @p declared is the
 * NUMINGROUP counter value; on entry @p it points at the first field after the counter.
 * On return @p it points beyond the group. Returns false if the visitor aborted the
 * walk (its own state carries the reason), true otherwise.
 *
 * The @p Visitor type (duck-typed to avoid std::function overhead on the hot path)
 * provides:
 *   void enter_instance(int depth, const group_def& def, size_t instance_index);
 *   bool field(int depth, const group_def& def, size_t instance_index, int member_index, const FixField& field);
 *   bool leave_instance(int depth, const group_def& def, size_t instance_index);
 *   bool orphan_field(int depth, const group_def& def, const FixField& field); // member seen before any delimiter
 *   bool enter_nested(int depth, const group_def& def, size_t instance_index, const FixField& counter, const group_def& nested);
 *        // return true to descend (the walker recurses with the same visitor at depth+1), false to skip the sub-group
 *   bool leave_group(int depth, const group_def& def, size_t instances, int declared);
 * Every bool-returning callback returns false to abort the whole walk.
 */
template <typename Visitor>
bool walk_repeating_group(const group_def& def, int declared, group_iterator& it, const group_iterator& end, int depth, Visitor& visitor) {
    size_t instances = 0;
    bool have_instance = false;

    while (it != end) {
        const FixField field = *it; // copy: *it aliases iterator-owned storage that ++it overwrites
        const int member = group_member_index(def, field.tag);
        if (member < 0) {
            break; // not a member of this group -> the group has ended
        }

        if (field.tag == def.delimiter_tag) {
            if (have_instance && !visitor.leave_instance(depth, def, instances - 1)) {
                return false;
            }
            visitor.enter_instance(depth, def, instances);
            ++instances;
            have_instance = true;
        } else if (!have_instance) {
            // A non-empty group instance must begin with its delimiter tag.
            if (!visitor.orphan_field(depth, def, field)) {
                return false;
            }
        }

        if (have_instance && !visitor.field(depth, def, instances - 1, member, field)) {
            return false;
        }

        const int nested = def.members[static_cast<size_t>(member)].nested_group;
        ++it;
        if (nested >= 0 && depth < max_group_depth) {
            const bool descend = have_instance && visitor.enter_nested(depth, def, instances - 1, field, group_at(nested));
            if (descend) {
                if (!walk_repeating_group(group_at(nested), parse_num_in_group(field.value), it, end, depth + 1, visitor)) {
                    return false;
                }
            } else {
                skip_repeating_group(group_at(nested), it, end);
            }
        }
    }

    if (have_instance && !visitor.leave_instance(depth, def, instances - 1)) {
        return false;
    }
    return visitor.leave_group(depth, def, instances, declared);
}

} // namespaces

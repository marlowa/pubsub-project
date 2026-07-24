#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <fix_codec/FixField.hpp>
#include <fix_codec/FixGroupWalker.hpp>
#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/FixReject.hpp>
#include <fix_codec/fix_dictionary.hpp>

namespace fix_codec {

/**
 * @brief Dictionary-driven validation layer over a framed FIX message.
 *
 * FixMessageReader is the tokeniser: it frames one message and exposes its fields
 * without copying or validating their meaning. FixMessageValidator is the layer
 * above it, and answers "is this message actually conformant?" using the metadata
 * the FIX dictionary generator emits (required tags per message, each field's data
 * format, and enumerated value sets). It allocates nothing and holds no state
 * between calls, so it is safe to construct on the hot path.
 *
 * It enforces six rules, each mapping to a FIX SessionRejectReason:
 *   - InvalidTagNumber (0)            -- a tag the dictionary does not define at all.
 *   - RequiredTagMissing (1)          -- a mandatory tag for this message type is absent.
 *   - TagNotDefinedForThisMessage (2) -- a defined tag not permitted in this message type.
 *   - ValueIsIncorrect (5)            -- an enumerated field carries an undefined value.
 *   - IncorrectDataFormat (6)         -- a value's text is not its field's FIX type.
 *   - TagAppearsMoreThanOnce (13)     -- a tag repeated where it may appear only once.
 *   - IncorrectNumInGroupCount (16)   -- a NUMINGROUP counter that does not match the
 *                                        number of group instances actually present.
 *
 * Validation is repeating-group aware. It walks the message once: top-level fields
 * are checked for definedness, permission, uniqueness, format and enum membership;
 * a NUMINGROUP counter tag opens a repeating group, whose instances are parsed with
 * their own scope. Within a group each instance begins with the group's delimiter
 * tag; a member tag is unique per instance (so a tag may legitimately recur across
 * instances -- for example UnderlyingSymbol in NoUnderlyings), required members must
 * be present in every instance, nested groups recurse, and the declared count must
 * equal the instances seen. The group structure is driven by the global group table
 * the dictionary generator emits (keyed by counter tag). Required-tag presence for
 * the message body (session header, top-level fields, required group counters) is
 * checked after the walk; required group members are checked per instance.
 */
class FixMessageValidator {
  public:
    explicit FixMessageValidator(const FixMessageReader& reader) : reader_(reader) {}

    /**
     * @brief Returns the first violation, or a reject with reason None if valid.
     *
     * Fields are checked in message order for duplicates, data format, and enum
     * membership; the required-tag check runs last. The first failure found is
     * returned, carrying the specific offending tag.
     */
    [[nodiscard]] FixReject validate() const {
        const std::string_view type = reader_.msg_type();
        // Resolve the message's permitted-tag bitset once; each field is then a
        // dense-index lookup plus a bit test. nullptr means the message type is
        // unknown, in which case per-field permission is not enforced.
        const uint64_t* const permitted = permitted_mask(type);

        int seen[max_tracked_tags];
        size_t seen_count = 0;
        auto it = reader_.begin();
        const auto end = reader_.end();
        while (it != end) {
            const FixField& field = *it;
            const int tag = field.tag;
            const int index = field_index(tag);
            if (index < 0) {
                return FixReject{RejectReason::InvalidTagNumber, tag, type, field.value};
            }
            if (permitted != nullptr && !mask_has_index(permitted, index)) {
                return FixReject{RejectReason::TagNotDefinedForThisMessage, tag, type, {}};
            }
            if (contains(seen, seen_count, tag)) {
                return FixReject{RejectReason::TagAppearsMoreThanOnce, tag, type, {}};
            }
            if (seen_count < max_tracked_tags) {
                seen[seen_count++] = tag;
            }
            // Reuse the dense index from field_index for the format and enum checks
            // so each is a direct array probe rather than another tag search.
            const FixReject semantic = check_field_semantics(index, tag, field, type);
            if (!semantic.ok()) {
                return semantic;
            }
            // A top-level NUMINGROUP counter opens a repeating group: descend into
            // its instances, which are validated with their own per-instance scope
            // (duplicate detection, required members, declared-vs-actual count).
            const int group = group_index_for_counter(tag);
            if (group >= 0) {
                const FixField counter = field; // copy before advancing: field aliases the iterator's storage
                ++it;
                GroupValidationVisitor visitor;
                visitor.type = type;
                visitor.counter_value[0] = counter.value;
                if (!walk_repeating_group(group_at(group), parse_num_in_group(counter.value), it, end, 0, visitor)) {
                    return visitor.reject; // the walker left `it` at the first field beyond the group
                }
                continue;
            }
            ++it;
        }

        // The top-level walk recorded every top-level tag in seen[] (unless the message
        // had more than max_tracked_tags fields). Check the required tags against
        // that set rather than re-scanning the message once per required tag.
        const bool tracked_all = seen_count < max_tracked_tags;
        for (const int required : required_tags(type)) {
            const bool present = tracked_all ? contains(seen, seen_count, required) : !reader_.find(required).empty();
            if (!present) {
                return FixReject{RejectReason::RequiredTagMissing, required, type, {}};
            }
        }
        return FixReject{};
    }

  private:
    // Enough for any realistic flat FIX message; extra fields skip duplicate
    // tracking rather than allocate, and are still format/enum checked.
    static constexpr size_t max_tracked_tags = 256;

    static bool contains(const int* values, size_t count, int target) {
        for (size_t index = 0; index < count; ++index) {
            if (values[index] == target) {
                return true;
            }
        }
        return false;
    }

    static FixReject check_format(int dense_index, int tag, const FixField& field, std::string_view type) {
        switch (field_format_at(dense_index)) {
            case field_format::fix_int:
                if (!is_integer(field.value)) {
                    return FixReject{RejectReason::IncorrectDataFormat, tag, type, field.value};
                }
                break;
            case field_format::fix_decimal: {
                int64_t mantissa = 0;
                int exponent = 0;
                if (!field.as_decimal(mantissa, exponent)) {
                    return FixReject{RejectReason::IncorrectDataFormat, tag, type, field.value};
                }
                break;
            }
            case field_format::fix_char:
                if (field.value.size() != 1) {
                    return FixReject{RejectReason::IncorrectDataFormat, tag, type, field.value};
                }
                break;
            case field_format::fix_boolean:
                if (field.value != "Y" && field.value != "N") {
                    return FixReject{RejectReason::IncorrectDataFormat, tag, type, field.value};
                }
                break;
            case field_format::fix_utc_timestamp:
                if (!is_utc_timestamp(field.value)) {
                    return FixReject{RejectReason::IncorrectDataFormat, tag, type, field.value};
                }
                break;
            case field_format::fix_string:
            case field_format::fix_data:
                break;
        }
        return FixReject{};
    }

    /** @brief FIX integer: an optional leading sign then at least one digit, whole view. */
    static bool is_integer(std::string_view text) {
        if (text.empty()) {
            return false;
        }
        size_t index = 0;
        if (text[index] == '+' || text[index] == '-') {
            ++index;
        }
        if (index == text.size()) {
            return false;
        }
        for (; index < text.size(); ++index) {
            if (text[index] < '0' || text[index] > '9') {
                return false;
            }
        }
        return true;
    }

    /** @brief FIX UTCTimestamp shape: "YYYYMMDD-HH:MM:SS" with an optional fraction. */
    static bool is_utc_timestamp(std::string_view text) {
        const FixField probe{0, text};
        // A valid timestamp never resolves to both distinct sentinels, so if either
        // parse returns something other than its own fallback the text is well-formed.
        return probe.as_utc_timestamp_ns(INT64_MIN) != INT64_MIN || probe.as_utc_timestamp_ns(INT64_MIN + 1) != INT64_MIN + 1;
    }

    /** @brief Data-format then enum-membership check for one field, by dense index. */
    static FixReject check_field_semantics(int dense_index, int tag, const FixField& field, std::string_view type) {
        const FixReject format_reject = check_format(dense_index, tag, field, type);
        if (!format_reject.ok()) {
            return format_reject;
        }
        if (has_enum_values_at(dense_index) && !is_defined_enum_value_at(dense_index, field.value)) {
            return FixReject{RejectReason::ValueIsIncorrect, tag, type, field.value};
        }
        return FixReject{};
    }

    /**
     * @brief walk_repeating_group visitor applying the per-instance validation rules:
     *        duplicate detection, required members, and declared-vs-actual count.
     *
     * One visitor drives every nesting level of a group, so its per-instance scope
     * (seen tags, their count, and the counter value for the reject text) is indexed by
     * the walk depth. The mechanical descent -- instance boundaries, nested recursion,
     * consuming beyond the group -- lives in fix_codec::walk_repeating_group.
     */
    struct GroupValidationVisitor {
        std::string_view type{};
        FixReject reject{};
        int instance_seen[max_group_depth + 1][max_tracked_tags]; // written before read; intentionally not zero-initialised
        size_t instance_count[max_group_depth + 1]{};
        std::string_view counter_value[max_group_depth + 1]{};

        void enter_instance(int depth, const group_def& /*def*/, size_t /*index*/) {
            instance_count[depth] = 0;
        }

        bool field(int depth, const group_def& /*def*/, size_t /*index*/, int /*member*/, const FixField& value) {
            int* const seen = instance_seen[depth];
            size_t& count = instance_count[depth];
            if (contains(seen, count, value.tag)) {
                reject = FixReject{RejectReason::TagAppearsMoreThanOnce, value.tag, type, {}};
                return false;
            }
            if (count < max_tracked_tags) {
                seen[count++] = value.tag;
            }
            const FixReject semantic = check_field_semantics(field_index(value.tag), value.tag, value, type);
            if (!semantic.ok()) {
                reject = semantic;
                return false;
            }
            return true;
        }

        bool orphan_field(int /*depth*/, const group_def& def, const FixField& /*value*/) {
            // A non-empty group instance must begin with its delimiter tag.
            reject = FixReject{RejectReason::RequiredTagMissing, def.delimiter_tag, type, {}};
            return false;
        }

        bool enter_nested(int depth, const group_def& /*def*/, size_t /*index*/, const FixField& counter, const group_def& /*nested*/) {
            counter_value[depth + 1] = counter.value; // for the nested group's IncorrectNumInGroupCount text
            return true;                              // always descend to validate the nested group
        }

        bool leave_instance(int depth, const group_def& def, size_t /*index*/) {
            // Every required member must be present in the just-closed instance.
            if (instance_count[depth] >= max_tracked_tags) {
                return true; // instance too large to have tracked -- skip, as the top-level walk does
            }
            for (size_t member = 0; member < def.member_count; ++member) {
                if (!def.members[member].required) {
                    continue;
                }
                if (!contains(instance_seen[depth], instance_count[depth], def.members[member].tag)) {
                    reject = FixReject{RejectReason::RequiredTagMissing, def.members[member].tag, type, {}};
                    return false;
                }
            }
            return true;
        }

        bool leave_group(int depth, const group_def& def, size_t instances, int declared) {
            if (static_cast<int>(instances) != declared) {
                reject = FixReject{RejectReason::IncorrectNumInGroupCount, def.counter_tag, type, counter_value[depth]};
                return false;
            }
            return true;
        }
    };

    const FixMessageReader& reader_;
};

} // namespaces

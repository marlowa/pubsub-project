#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <fix_codec/FixField.hpp>
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
                const FixReject group_reject = parse_group(group_at(group), counter, type, it, end, 0);
                if (!group_reject.ok()) {
                    return group_reject;
                }
                continue; // parse_group left `it` at the first field beyond the group
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

    // Guards against a pathological (cyclic) dictionary; real FIX group nesting is
    // only a few levels deep. Beyond this, a nested counter is treated as a plain
    // field rather than recursed, bounding stack use.
    static constexpr int max_group_depth = 16;

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

    /** @brief Index of @p tag within a group body, or -1 if it is not a member. */
    static int group_member_index(const group_def& def, int tag) {
        for (size_t member = 0; member < def.member_count; ++member) {
            if (def.members[member].tag == tag) {
                return static_cast<int>(member);
            }
        }
        return -1;
    }

    /** @brief Non-negative NUMINGROUP count from a (format-validated) integer value. */
    static int parse_num_in_group(std::string_view text) {
        int value = 0;
        for (const char character : text) {
            if (character < '0' || character > '9') {
                continue; // skip a leading sign; the field was already format-checked
            }
            value = value * 10 + (character - '0');
        }
        return value;
    }

    /** @brief Every required member of @p def must be present in the just-closed instance. */
    static FixReject check_instance_required(const group_def& def, const int* instance_seen, size_t instance_count, std::string_view type) {
        if (instance_count >= max_tracked_tags) {
            return FixReject{}; // instance too large to have tracked -- skip, as the top-level walk does
        }
        for (size_t member = 0; member < def.member_count; ++member) {
            if (!def.members[member].required) {
                continue;
            }
            if (!contains(instance_seen, instance_count, def.members[member].tag)) {
                return FixReject{RejectReason::RequiredTagMissing, def.members[member].tag, type, {}};
            }
        }
        return FixReject{};
    }

    /**
     * @brief Validate the instances of one repeating group.
     *
     * On entry @p it points at the first field after the NUMINGROUP counter; on
     * return it points at the first field that is not part of this group. Each
     * instance begins with @c def.delimiter_tag; within an instance every member
     * tag is unique, format/enum checked, and required members must be present, and
     * a nested-group counter recurses. Finally the declared count (@p counter_field)
     * must equal the number of instances actually seen.
     */
    static FixReject parse_group(const group_def& def, const FixField& counter_field, std::string_view type, FixMessageReader::const_iterator& it,
                                 const FixMessageReader::const_iterator& end, int depth) {
        const int declared = parse_num_in_group(counter_field.value);

        int instance_seen[max_tracked_tags];
        size_t instance_count = 0;
        int instances = 0;

        while (it != end) {
            const FixField& field = *it;
            const int tag = field.tag;
            const int member = group_member_index(def, tag);
            if (member < 0) {
                break; // not a member of this group -> the group has ended
            }

            if (tag == def.delimiter_tag) {
                // A new instance begins. Close the previous instance's required check.
                if (instances > 0) {
                    const FixReject missing = check_instance_required(def, instance_seen, instance_count, type);
                    if (!missing.ok()) {
                        return missing;
                    }
                }
                ++instances;
                instance_count = 0;
            } else if (instances == 0) {
                // A non-empty group instance must begin with its delimiter tag.
                return FixReject{RejectReason::RequiredTagMissing, def.delimiter_tag, type, {}};
            }

            if (contains(instance_seen, instance_count, tag)) {
                return FixReject{RejectReason::TagAppearsMoreThanOnce, tag, type, {}};
            }
            if (instance_count < max_tracked_tags) {
                instance_seen[instance_count++] = tag;
            }

            const int index = field_index(tag);
            const FixReject semantic = check_field_semantics(index, tag, field, type);
            if (!semantic.ok()) {
                return semantic;
            }

            const int nested = def.members[static_cast<size_t>(member)].nested_group;
            if (nested >= 0 && depth < max_group_depth) {
                const FixField nested_counter = field; // copy before advancing: field aliases the iterator's storage
                ++it;
                const FixReject nested_reject = parse_group(group_at(nested), nested_counter, type, it, end, depth + 1);
                if (!nested_reject.ok()) {
                    return nested_reject;
                }
                continue; // parse_group left `it` at the first field beyond the nested group
            }
            ++it;
        }

        if (instances > 0) {
            const FixReject missing = check_instance_required(def, instance_seen, instance_count, type);
            if (!missing.ok()) {
                return missing;
            }
        }
        if (instances != declared) {
            return FixReject{RejectReason::IncorrectNumInGroupCount, def.counter_tag, type, counter_field.value};
        }
        return FixReject{};
    }

    const FixMessageReader& reader_;
};

} // namespaces

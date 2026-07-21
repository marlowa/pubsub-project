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
 *   - TagAppearsMoreThanOnce (13)     -- a tag is repeated.
 *
 * Duplicate detection treats any repeated tag as an error. This is correct for
 * messages without repeating groups (the flat session and order messages this
 * system exchanges, such as NewOrderSingle); repeating-group awareness -- where a
 * group member may legitimately recur -- is a later enhancement and would need the
 * per-message group structure, deliberately not emitted here to keep the generated
 * header small and validation fast.
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
        for (const FixField& field : reader_) {
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
            const FixReject format_reject = check_format(index, tag, field, type);
            if (!format_reject.ok()) {
                return format_reject;
            }
            if (has_enum_values_at(index) && !is_defined_enum_value_at(index, field.value)) {
                return FixReject{RejectReason::ValueIsIncorrect, tag, type, field.value};
            }
        }

        // The single walk above recorded every tag in seen[] (unless the message
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

    const FixMessageReader& reader_;
};

} // namespaces

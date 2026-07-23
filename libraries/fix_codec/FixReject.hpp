#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdio>
#include <string_view>

#include <fix_codec/fix_dictionary.hpp>

namespace fix_codec {

/**
 * @brief Why a FIX message failed validation.
 *
 * The numeric values are the FIX SessionRejectReason (tag 373) codes, so a reject
 * can be turned into a FIX Reject (35=3) message directly. A single integer code
 * is not enough on its own to tell a human or an operator what went wrong, which
 * is why @ref FixReject also carries the offending tag and message type.
 */
enum class RejectReason : int {
    None = -1,
    InvalidTagNumber = 0, ///< A tag the dictionary does not define at all.
    RequiredTagMissing = 1,
    TagNotDefinedForThisMessage = 2, ///< A defined tag not permitted in this message type.
    ValueIsIncorrect = 5,            ///< A value not defined for an enumerated field.
    IncorrectDataFormat = 6,         ///< A value whose text is not the field's FIX type.
    TagAppearsMoreThanOnce = 13,     ///< A tag repeated where it may appear only once.
    IncorrectNumInGroupCount = 16,   ///< A NUMINGROUP counter that does not match the instances present.
};

/** @brief The reason name, used in diagnostics (never allocates). */
inline constexpr std::string_view reason_text(RejectReason reason) {
    switch (reason) {
        case RejectReason::None:
            return "None";
        case RejectReason::InvalidTagNumber:
            return "InvalidTagNumber";
        case RejectReason::RequiredTagMissing:
            return "RequiredTagMissing";
        case RejectReason::TagNotDefinedForThisMessage:
            return "TagNotDefinedForThisMessage";
        case RejectReason::ValueIsIncorrect:
            return "ValueIsIncorrect";
        case RejectReason::IncorrectDataFormat:
            return "IncorrectDataFormat";
        case RejectReason::TagAppearsMoreThanOnce:
            return "TagAppearsMoreThanOnce";
        case RejectReason::IncorrectNumInGroupCount:
            return "IncorrectNumInGroupCount";
    }
    return "Unknown";
}

/**
 * @brief One validation failure: the reason, the specific offending tag, the
 *        message type, and (where relevant) the offending value.
 *
 * The tag number is what makes the error specific -- "which tag was missing" or
 * "which tag was duplicated" is answered by @ref ref_tag, and @ref describe turns
 * that number into a name via the generated tag_name() table. These four members
 * are exactly what a FIX Reject (35=3) carries: 373 (reason), 371 (ref_tag),
 * 372 (ref_msg_type) and 58 (the text @ref describe produces).
 */
struct FixReject {
    RejectReason reason{RejectReason::None};
    int ref_tag{0};
    std::string_view ref_msg_type{};
    std::string_view value{};

    /** @brief True when this reject represents "no error". */
    [[nodiscard]] bool ok() const {
        return reason == RejectReason::None;
    }

    /**
     * @brief Writes a human-readable description into @p buffer, no allocation.
     *
     * Renders the offending tag both as a number and, via the generated
     * tag_name() lookup, as its canonical FIX name, for example:
     *   "RequiredTagMissing: tag 11 (ClOrdID) in NewOrderSingle(D)"
     *   "IncorrectDataFormat: tag 38 (OrderQty) value '12abc' in D"
     * Returns a view of the written bytes (a prefix of @p buffer).
     */
    [[nodiscard]] std::string_view describe(char* buffer, size_t capacity) const {
        if (buffer == nullptr || capacity == 0) {
            return {};
        }
        const std::string_view reason_name = reason_text(reason);
        const std::string_view tag_text = tag_name(ref_tag);
        int written = 0;
        if (value.empty()) {
            written = std::snprintf(buffer, capacity, "%.*s: tag %d (%.*s) in %.*s", static_cast<int>(reason_name.size()), reason_name.data(), ref_tag,
                                    static_cast<int>(tag_text.size()), tag_text.data(), static_cast<int>(ref_msg_type.size()), ref_msg_type.data());
        } else {
            written = std::snprintf(buffer, capacity, "%.*s: tag %d (%.*s) value '%.*s' in %.*s", static_cast<int>(reason_name.size()), reason_name.data(),
                                    ref_tag, static_cast<int>(tag_text.size()), tag_text.data(), static_cast<int>(value.size()), value.data(),
                                    static_cast<int>(ref_msg_type.size()), ref_msg_type.data());
        }
        if (written < 0) {
            return {};
        }
        const size_t length = static_cast<size_t>(written) < capacity ? static_cast<size_t>(written) : capacity - 1;
        return {buffer, length};
    }
};

} // namespaces

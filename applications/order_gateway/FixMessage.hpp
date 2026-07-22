#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fix_codec/fix_dictionary.hpp>

namespace order_gateway {

/**
 * @brief A simple container for a single FIX message's tag/value pairs.
 *
 * FixMessage holds the fields of one complete FIX message as an ordered list of
 * (tag number, string value) pairs -- insertion order is preserved so the
 * serialiser can emit them in order. It does not validate field presence or types
 * -- that is the responsibility of the code that consumes the message.
 *
 * This is intentionally minimal and scoped to the sample FIX gateway. It is
 * not a general-purpose FIX message implementation.
 *
 * FIX tag numbers referenced in this sample:
 *   8  -- BeginString (e.g. "FIXT.1.1")
 *   9  -- BodyLength
 *   10 -- Checksum
 *   11 -- ClOrdID
 *   14 -- CumQty
 *   17 -- ExecID
 *   34 -- MsgSeqNum
 *   35 -- MsgType
 *   37 -- OrderID
 *   38 -- OrderQty
 *   39 -- OrdStatus
 *   40 -- OrdType
 *   44 -- Price
 *   49 -- SenderCompID
 *   52 -- SendingTime
 *   54 -- Side
 *   55 -- Symbol
 *   56 -- TargetCompID
 *   58 -- Text
 *   98 -- EncryptMethod
 *   108 -- HeartBtInt
 *   150 -- ExecType
 *   151 -- LeavesQty
 */
class FixMessage {
  public:
    using Field = std::pair<int, std::string>;

    FixMessage() = default;

    /**
     * @brief Sets a field by tag number and string value.
     *
     * A tag set for the first time is appended; setting an existing tag overwrites
     * its value in place. Insertion order is preserved so a serialiser can emit the
     * fields in the order they were set, rather than from a hand-maintained list.
     */
    void set(int tag, const std::string& value) {
        field_for(tag) = value;
    }

    /**
     * @brief Sets a field by tag number and string_view value (copies into storage).
     *
     * Provided so outbound FixMessage instances can be populated directly from
     * ParsedFixMessage::get() return values without an explicit conversion to
     * std::string at each call site.
     */
    void set(int tag, std::string_view value) {
        field_for(tag).assign(value.data(), value.size());
    }

    /**
     * @brief Sets a field by tag number and integer value.
     */
    void set(int tag, int value) {
        field_for(tag) = std::to_string(value);
    }

    /**
     * @brief Returns the value for the given tag, or empty string if not present.
     */
    [[nodiscard]] const std::string& get(int tag) const {
        static const std::string empty;
        for (const Field& field : fields_) {
            if (field.first == tag) {
                return field.second;
            }
        }
        return empty;
    }

    /**
     * @brief Returns true if the given tag is present in the message.
     */
    [[nodiscard]] bool has(int tag) const {
        for (const Field& field : fields_) {
            if (field.first == tag) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Returns the MsgType field (tag 35), or empty string if not present.
     */
    [[nodiscard]] const std::string& msg_type() const {
        return get(35);
    }

    /**
     * @brief The fields in the order they were set. Used by the serialiser to emit
     *        every field the caller supplied without a hand-maintained tag list.
     */
    [[nodiscard]] const std::vector<Field>& fields() const {
        return fields_;
    }

    /**
     * @brief Clears all fields. Used to reset the message for reuse.
     */
    void clear() {
        fields_.clear();
    }

    /**
     * @brief Returns the number of fields in the message.
     */
    [[nodiscard]] int size() const {
        return static_cast<int>(fields_.size());
    }

  private:
    std::string& field_for(int tag) {
        for (Field& field : fields_) {
            if (field.first == tag) {
                return field.second;
            }
        }
        fields_.emplace_back(tag, std::string());
        return fields_.back().second;
    }

    std::vector<Field> fields_;
};

// Tag numbers and MsgType values are the generated FIX dictionary constants; the
// hand-maintained tables were deleted in the fix_codec migration (stage 1). These
// aliases keep the existing Tag::/MsgType:: call sites unchanged. Note that the
// MsgType values are now std::string_view (not std::string) -- every use is a
// comparison or FixMessage::set(int, std::string_view), both of which accept a view.
namespace MsgType = fix_codec::msg_type;
namespace Tag = fix_codec::tag;

/**
 * @brief View-based representation of one complete inbound FIX message.
 *
 * All string_view values point directly into the MirroredBuffer that holds the
 * raw TCP bytes for this connection. They are valid only for the duration of
 * the FixParser message callback. The object must not be copied or moved out
 * of the callback scope.
 *
 * Field values are stored in a flat array rather than a hash map. Linear scan
 * over the (small) number of fields in a typical FIX message is faster than
 * any hash-based lookup at this cardinality, and allocates nothing.
 *
 * Field count: any real FIX message has far fewer than 64 tag-value pairs.
 * Fields beyond that limit are silently ignored; this never occurs for the
 * message types handled by this gateway.
 */
struct ParsedFixMessage {
    struct Field {
        int tag;
        std::string_view value;
    };

    static constexpr int maximum_field_count = 64;

    std::array<Field, maximum_field_count> fields{};
    int field_count{0};

    void set(int tag, std::string_view value) {
        if (field_count < maximum_field_count) {
            fields[static_cast<size_t>(field_count++)] = {tag, value};
        }
    }

    [[nodiscard]] std::string_view get(int tag) const {
        for (int i = 0; i < field_count; ++i) {
            if (fields[static_cast<size_t>(i)].tag == tag) {
                return fields[static_cast<size_t>(i)].value;
            }
        }
        return {};
    }

    [[nodiscard]] bool has(int tag) const {
        for (int i = 0; i < field_count; ++i) {
            if (fields[static_cast<size_t>(i)].tag == tag) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::string_view msg_type() const {
        return get(Tag::MsgType);
    }

    [[nodiscard]] int size() const {
        return field_count;
    }

    // Non-copyable, non-movable: string_views are only valid during the
    // on_message_ callback. Deleting these operations prevents the object from
    // escaping the callback's stack frame.
    ParsedFixMessage() = default;
    ParsedFixMessage(const ParsedFixMessage&) = delete;
    ParsedFixMessage& operator=(const ParsedFixMessage&) = delete;
    ParsedFixMessage(ParsedFixMessage&&) = delete;
    ParsedFixMessage& operator=(ParsedFixMessage&&) = delete;
};

/**
 * @brief The expected byte sequence at the start of every inbound FIX 5.0SP2
 *        / FIXT 1.1 message stream.
 *
 * Any inbound connection whose first bytes do not match this preamble is not
 * a valid FIX FIXT.1.1 client and should be disconnected immediately.
 *
 * The length is computed at compile time via string_view::size() -- no magic
 * numbers are needed in code that checks the preamble.
 */
static constexpr std::string_view expected_preamble = "8=FIXT.1.1\x01";

} // namespaces

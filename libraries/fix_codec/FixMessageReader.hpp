#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <fix_codec/FixChecksum.hpp>
#include <fix_codec/FixField.hpp>
#include <fix_codec/fix_dictionary.hpp>

namespace fix_codec {

/**
 * @brief Zero-copy reader for one FIX message at the start of a byte window.
 *
 * FixMessageReader is constructed over a borrowed byte window (the same shape
 * FixParser::feed receives from a MirroredBuffer) whose first bytes are the
 * start of a FIX message (tag 8, BeginString). It frames exactly one message
 * using BodyLength (tag 9) -- so it never scans for the checksum tag -- reports
 * whether that message is complete and its checksum valid, and exposes the
 * fields as a forward range of FixField without copying any bytes.
 *
 * Following hffix, no memory is allocated: the fields are string_views into the
 * window, valid only while the window is alive and unmodified.
 *
 * Stream framing (skipping leading garbage, resynchronising after a corrupt
 * message) is the caller's responsibility. A well-formed producer places a
 * message boundary at the window start; @ref status reports Malformed otherwise.
 */
class FixMessageReader {
  public:
    enum class Status {
        Valid,         ///< A complete message with a correct checksum.
        Incomplete,    ///< The window does not yet hold the whole message.
        Malformed,     ///< The framing is wrong (not a FIX message boundary).
        ChecksumError, ///< The message is complete but its checksum is wrong.
    };

    FixMessageReader(const char* buffer, size_t size) : window_(buffer, size) {
        frame();
    }

    explicit FixMessageReader(std::string_view window) : window_(window) {
        frame();
    }

    [[nodiscard]] Status status() const {
        return status_;
    }

    [[nodiscard]] bool is_valid() const {
        return status_ == Status::Valid;
    }

    /**
     * @brief A human-readable explanation of why framing did not yield a valid
     *        message, or std::nullopt when the message is Valid.
     *
     * Different framing failures give different text -- a non-numeric BodyLength, a
     * negative BodyLength, and a CheckSum tag at the wrong offset are each reported
     * distinctly -- so a caller can log or reject with a specific reason rather than
     * a bare Status. Framing itself stores only a static view and never allocates;
     * the std::string is built here, on the error path only, when the caller asks.
     */
    [[nodiscard]] std::optional<std::string> error() const {
        if (error_message_.empty()) {
            return std::nullopt;
        }
        return std::string(error_message_);
    }

    /**
     * @brief Number of bytes this message occupies in the window.
     *
     * Non-zero once the message is fully present (Valid or ChecksumError), so a
     * stream driver can advance its read position by this amount even when the
     * checksum is wrong. Zero for Incomplete or Malformed.
     */
    [[nodiscard]] size_t message_size() const {
        return message_bytes_.size();
    }

    /** @brief The complete framed message bytes (empty unless fully present). */
    [[nodiscard]] std::string_view message_bytes() const {
        return message_bytes_;
    }

    /**
     * @brief Forward iterator over the message's tag=value fields.
     *
     * Dereferences to a FixField. Honours data-length fields: a field whose tag
     * is a LENGTH tag paired with a DATA field causes the following DATA field to
     * be read by exact byte count rather than by scanning for SOH, so a DATA
     * value may itself contain the SOH delimiter.
     */
    class const_iterator {
      public:
        using value_type = FixField;
        using reference = const FixField&;
        using pointer = const FixField*;

        const_iterator(const char* position, const char* end, size_t incoming_data_length) : field_start_(position), end_(end) {
            parse(incoming_data_length);
        }

        reference operator*() const {
            return field_;
        }

        pointer operator->() const {
            return &field_;
        }

        const_iterator& operator++() {
            field_start_ = next_;
            parse(pending_data_length_);
            return *this;
        }

        bool operator==(const const_iterator& other) const {
            return field_start_ == other.field_start_;
        }

        bool operator!=(const const_iterator& other) const {
            return field_start_ != other.field_start_;
        }

      private:
        void parse(size_t incoming_data_length) {
            field_ = FixField{};
            pending_data_length_ = 0;
            if (field_start_ >= end_) {
                field_start_ = end_;
                next_ = end_;
                return;
            }
            const char* equals = static_cast<const char*>(find_char(field_start_, end_, '='));
            if (equals == nullptr) {
                field_start_ = end_;
                next_ = end_;
                return;
            }
            const std::string_view tag_text(field_start_, static_cast<size_t>(equals - field_start_));
            const char* const value_start = equals + 1;
            const char* value_end = nullptr;
            if (incoming_data_length > 0 && value_start + incoming_data_length <= end_) {
                value_end = value_start + incoming_data_length;
            } else {
                value_end = static_cast<const char*>(find_char(value_start, end_, soh));
            }
            if (value_end == nullptr || value_end >= end_) {
                field_start_ = end_;
                next_ = end_;
                return;
            }
            field_.tag = detail::parse_number<int>(tag_text, 0);
            field_.value = std::string_view(value_start, static_cast<size_t>(value_end - value_start));
            if (field_.tag != 0 && is_data_length_tag(field_.tag)) {
                pending_data_length_ = static_cast<size_t>(field_.as_int(0));
            }
            next_ = value_end + 1; // step past the SOH terminating this field
        }

        static const char* find_char(const char* first, const char* last, char target) {
            for (const char* cursor = first; cursor < last; ++cursor) {
                if (*cursor == target) {
                    return cursor;
                }
            }
            return nullptr;
        }

        const char* field_start_;
        const char* end_;
        const char* next_{nullptr};
        size_t pending_data_length_{0};
        FixField field_{};
    };

    [[nodiscard]] const_iterator begin() const {
        return const_iterator(fields_begin_, fields_end_, 0);
    }

    [[nodiscard]] const_iterator end() const {
        return const_iterator(fields_end_, fields_end_, 0);
    }

    /** @brief Returns the first field with the given tag, or an empty FixField. */
    [[nodiscard]] FixField find(int tag) const {
        return find(tag, begin());
    }

    /**
     * @brief Returns the first field with the given tag at or after @p hint.
     *
     * FIX fields arrive in a known order, so resuming a search from the previous
     * result (hffix's find_with_hint) avoids rescanning the header each time.
     */
    [[nodiscard]] FixField find(int tag, const_iterator hint) const {
        for (const_iterator it = hint; it != end(); ++it) {
            if (it->tag == tag) {
                return *it;
            }
        }
        return FixField{};
    }

    /** @brief The MsgType (tag 35) value, or an empty view if absent. */
    [[nodiscard]] std::string_view msg_type() const {
        return find(tag::MsgType).value;
    }

  private:
    static constexpr char soh = '\x01';

    static size_t find_delimiter(std::string_view text, size_t from) {
        for (size_t index = from; index < text.size(); ++index) {
            if (text[index] == soh) {
                return index;
            }
        }
        return std::string_view::npos;
    }

    void frame() {
        // Tag 8 (BeginString) must open the window.
        if (window_.size() < 2) {
            fail(Status::Incomplete, "message truncated before the BeginString (tag 8) field");
            return;
        }
        if (window_.compare(0, 2, "8=") != 0) {
            fail(Status::Malformed, "message does not start with tag 8 (BeginString)");
            return;
        }
        const size_t begin_string_end = find_delimiter(window_, 0);
        if (begin_string_end == std::string_view::npos) {
            fail(Status::Incomplete, "BeginString (tag 8) field is not terminated by SOH");
            return;
        }
        // Tag 9 (BodyLength) must be the second field.
        const size_t body_length_tag_start = begin_string_end + 1;
        if (window_.size() < body_length_tag_start + 2) {
            fail(Status::Incomplete, "message truncated before the BodyLength (tag 9) field");
            return;
        }
        if (window_.compare(body_length_tag_start, 2, "9=") != 0) {
            fail(Status::Malformed, "tag 9 (BodyLength) must be the second field");
            return;
        }
        const size_t body_length_end = find_delimiter(window_, body_length_tag_start);
        if (body_length_end == std::string_view::npos) {
            fail(Status::Incomplete, "BodyLength (tag 9) field is not terminated by SOH");
            return;
        }
        const std::string_view body_length_text = window_.substr(body_length_tag_start + 2, body_length_end - body_length_tag_start - 2);
        // Two fallbacks distinguish a non-numeric BodyLength (parse fails, the two
        // results differ) from a negative one (parse succeeds with a value <= 0).
        const int body_length = detail::parse_number<int>(body_length_text, -1);
        if (body_length != detail::parse_number<int>(body_length_text, -2)) {
            fail(Status::Malformed, "BodyLength (tag 9) is not a number");
            return;
        }
        if (body_length <= 0) {
            fail(Status::Malformed, "BodyLength (tag 9) must be a positive integer");
            return;
        }
        frame_checksum(body_length_end, static_cast<size_t>(body_length));
    }

    void frame_checksum(size_t body_length_end, size_t body_length) {
        // BodyLength counts from just after the tag 9 SOH to just before the tag 10 SOH.
        const size_t checksum_tag_start = body_length_end + 1 + body_length;
        if (window_.size() < checksum_tag_start + 3) {
            fail(Status::Incomplete, "message truncated before the CheckSum (tag 10) field");
            return;
        }
        if (window_.compare(checksum_tag_start, 3, "10=") != 0) {
            fail(Status::Malformed, "tag 10 (CheckSum) is not at the offset BodyLength indicates");
            return;
        }
        const size_t checksum_end = find_delimiter(window_, checksum_tag_start);
        if (checksum_end == std::string_view::npos) {
            fail(Status::Incomplete, "CheckSum (tag 10) field is not terminated by SOH");
            return;
        }
        message_bytes_ = window_.substr(0, checksum_end + 1);
        fields_begin_ = message_bytes_.data();
        fields_end_ = message_bytes_.data() + message_bytes_.size();

        const std::string_view checksum_input = window_.substr(0, checksum_tag_start);
        const std::string_view received = window_.substr(checksum_tag_start + 3, checksum_end - checksum_tag_start - 3);
        if (checksum_matches(checksum_input, received)) {
            status_ = Status::Valid;
        } else {
            fail(Status::ChecksumError, "CheckSum (tag 10) does not match the computed value");
        }
    }

    // Records the framing outcome and its explanation. error_message_ is a static
    // view, so this never allocates; the std::string is built only in error().
    void fail(Status status, std::string_view explanation) {
        status_ = status;
        error_message_ = explanation;
    }

    std::string_view window_;
    std::string_view message_bytes_{};
    const char* fields_begin_{nullptr};
    const char* fields_end_{nullptr};
    Status status_{Status::Incomplete};
    std::string_view error_message_{};
};

} // namespaces

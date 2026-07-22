// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixParser.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <fix_codec/FixField.hpp>
#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/FixMessageValidator.hpp>
#include <fix_codec/FixReject.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace order_gateway {

namespace {
constexpr std::string_view begin_string_tag = "8=";
} // namespaces

FixParser::FixParser(pubsub_itc_fw::QuillLogger& logger, MessageCallback on_message, RejectCallback on_reject)
    : on_message_(std::move(on_message)), on_reject_(std::move(on_reject)), logger_(logger) {}

size_t FixParser::feed(const uint8_t* data, size_t available) {
    // The double-mapping of MirroredBuffer guarantees this region is contiguous in
    // virtual address space even if the ring's physical storage wraps around.
    const std::string_view window(reinterpret_cast<const char*>(data), available);

    // committed is the number of bytes fully consumed by complete (or definitively
    // skipped) messages. The caller advances the MirroredBuffer read pointer by it.
    // Framing, checksum validation, and field extraction are delegated to
    // fix_codec::FixMessageReader; this loop only drives it across the stream and
    // resynchronises on malformed input, reproducing the previous parser's contract.
    size_t committed = 0;
    size_t cursor = 0;

    while (true) {
        const size_t start = window.find(begin_string_tag, cursor);
        if (start == std::string_view::npos) {
            break; // no further message start; retain any unconsumed bytes
        }

        const fix_codec::FixMessageReader reader(window.substr(start));
        switch (reader.status()) {
            case fix_codec::FixMessageReader::Status::Incomplete:
                // The message at start is not yet fully present. Stop and leave it (and
                // any preceding bytes) in the ring for the next recv().
                return committed;

            case fix_codec::FixMessageReader::Status::Malformed: {
                const std::optional<std::string> reason = reader.error();
                PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                           "FixParser: malformed FIX message at window offset {} ({}) -- skipping one byte and resyncing", start,
                           reason ? std::string_view(*reason) : std::string_view("unknown"));
                committed = start + 1;
                cursor = start + 1;
                break;
            }

            case fix_codec::FixMessageReader::Status::ChecksumError: {
                const std::optional<std::string> reason = reader.error();
                PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning, "FixParser: {} -- discarding message of {} bytes",
                           reason ? std::string_view(*reason) : std::string_view("bad checksum"), reader.message_size());
                committed = start + reader.message_size();
                cursor = committed;
                break;
            }

            case fix_codec::FixMessageReader::Status::Valid: {
                // Field values are string_views into the window -- zero copy, valid only
                // for the duration of the callback.
                ParsedFixMessage message;
                for (const fix_codec::FixField& field : reader) {
                    message.set(field.tag, field.value);
                }
                // A well-framed message with no MsgType (tag 35) is garbled per FIX
                // (MsgType must be the third field); discard it silently like a bad
                // checksum -- do not validate or reject it.
                if (message.has(Tag::MsgType)) {
                    const fix_codec::FixReject reject = fix_codec::FixMessageValidator(reader).validate();
                    if (reject.ok()) {
                        on_message_(message);
                    } else {
                        on_reject_(message, reject);
                    }
                }
                committed = start + reader.message_size();
                cursor = committed;
                break;
            }
        }
    }

    return committed;
}

void FixParser::reset() {
    // Nothing to reset: the parser has no internal accumulation buffer. Partial
    // bytes in the MirroredBuffer are discarded when the caller advances the ring's
    // read pointer fully on connection close.
}

} // namespaces

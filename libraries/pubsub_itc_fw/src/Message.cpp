// C headers including posix API headers
// (None directly required for this implementation)

// C++ headers whose names start with ‘c’
#include <cstdint> // For uint8_t

// System C++ headers
// (No specific system C++ headers are needed beyond what's included by the header)
#include <string>

// Third party headers
// none yet

// Project headers
#include <pubsub_itc_fw/Message.hpp> // The header for the Message class
#include <pubsub_itc_fw/utils/SimpleSpan.hpp>

namespace pubsub_itc_fw {

Message::Message(const std::string& topic, utils::SimpleSpan<uint8_t> payload_span, int64_t sequence_number)
    : topic_(topic), payload_(payload_span), sequence_number_(sequence_number) {}

// Accessor for the topic
const std::string& Message::get_topic() const {
    return topic_;
}

// Accessor for the payload
const utils::SimpleSpan<uint8_t>& Message::get_payload() const {
    return payload_;
}

// Accessor for the sequence number
int64_t Message::get_sequence_number() const {
    return sequence_number_;
}

} // namespace pubsub_itc_fw

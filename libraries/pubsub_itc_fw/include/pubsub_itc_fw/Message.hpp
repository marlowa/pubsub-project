#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <pubsub_itc_fw/utils/SimpleSpan.hpp>

namespace pubsub_itc_fw {

/** @ingroup messaging_subsystem */

/**
 * @brief Represents a message in the pubsub framework.
 *
 * This class encapsulates the data transmitted between threads, including
 * a payload, topic, and a unique sequence number for handling late subscribers.
 */
class Message  {
public:
    ~Message() = default;

    /**
     * @brief Constructs a message with a payload and topic.
     *
     * @param [in] topic The topic of the message.
     * @param [in] payload_span A SimpleSpan representing the message's payload.
     * @param [in] sequence_number The unique sequence number of the message.
     */
    explicit Message(const std::string& topic, utils::SimpleSpan<uint8_t> payload_span, int64_t sequence_number);

    /**
     * @brief Gets the message's topic.
     * @returns A const reference to the message's topic.
     */
    [[nodiscard]] const std::string& get_topic() const;

    /**
     * @brief Gets the message's payload.
     * @returns A const reference to the message's payload span.
     */
    [[nodiscard]] const utils::SimpleSpan<uint8_t>& get_payload() const;

    /**
     * @brief Gets the message's sequence number.
     * @returns The message's unique sequence number.
     */
    [[nodiscard]] int64_t get_sequence_number() const;

private:
    std::string topic_;
    utils::SimpleSpan<uint8_t> payload_;
    int64_t sequence_number_;
};

} // namespace pubsub_itc_fw

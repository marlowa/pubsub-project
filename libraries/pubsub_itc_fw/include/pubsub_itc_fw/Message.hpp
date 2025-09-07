#pragma once

// C headers including posix API headers
// (None directly here)

// C++ headers whose names start with ‘c’
#include <cstdint> // For uint8_t

// System C++ headers
#include <string_view> // For std::string_view
#include <utility>     // For std::move

// Third party headers
// (None directly here)

// Project headers
// (None directly here)

namespace pubsub_itc_fw {

/**
 * @brief A lightweight, non-owning wrapper for binary message payloads.
 *
 * This class serves as a generic data carrier for both pubsub and inter-thread
 * communication (ITC) messages. It holds a view (span) over raw binary data
 * and its length. The actual data memory is managed externally (e.g., by memory pools).
 *
 * This design is fully compatible with Google Protobuf. On the sending side, you serialize
 * your Protobuf message into a buffer (from a memory pool), then create a `Message`
 * that references this buffer. On the receiving side, you use the `Message`'s
 * data to deserialize into a Protobuf message.
 */
class Message final {
  public:
    /**
     * @brief Constructs a Message from a span of binary data.
     * @param [in] data A span providing a view to the binary data.
     */
    explicit Message(std::string_view data) : data_span_(data) {}

    /**
     * @brief Copy constructor.
     */
    Message(const Message& other) = default;

    /**
     * @brief Move constructor.
     */
    Message(Message&& other) noexcept = default;

    /**
     * @brief Copy assignment operator.
     */
    Message& operator=(const Message& other) = default;

    /**
     * @brief Move assignment operator.
     */
    Message& operator=(Message&& other) noexcept = default;

    /**
     * @brief Returns a `std::string_view` to the binary data payload.
     * @return A `std::string_view` representing the message data.
     */
    [[nodiscard]] std::string_view get_data() const {
        return data_span_;
    }

    /**
     * @brief Returns the length of the binary data payload.
     * @return The length of the message data in bytes, as a `size_t`.
     */
    [[nodiscard]] size_t get_length() const {
        return data_span_.length();
    }

  private:
    std::string_view data_span_;
};

} // namespace pubsub_itc_fw

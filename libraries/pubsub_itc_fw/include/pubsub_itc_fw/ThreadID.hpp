#pragma once

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <atomic>
#include <cstdint>
#include <functional> // For std::hash
#include <iosfwd>     // For std::ostream
#include <thread>     // For std::thread::id

// Third party headers
// (None directly here)

// Project headers
// (None directly here)

namespace pubsub_itc_fw {

/**
 * @brief Represents a unique identifier for a thread in the framework.
 *
 * This class wraps a thread ID, providing a consistent way to identify threads
 * within the PubSub ITC framework. It provides methods for creation, comparison,
 * and retrieval of the underlying ID.
 */
class ThreadID final {
  public:
    /**
     * @brief Default constructor, creates an invalid thread ID.
     */
    ThreadID() : id_(0) {}

    /**
     * @brief Constructs a ThreadID from a unique integer identifier.
     * @param [in] id The integer ID of the thread.
     */
    explicit ThreadID(uint32_t id) : id_(id) {}

    /**
     * @brief Checks if the ThreadID is valid.
     * @return `true` if the ID is not zero, `false` otherwise.
     */
    [[nodiscard]] bool is_valid() const {
        return id_ != 0;
    }

    /**
     * @brief Retrieves the integer value of the thread ID.
     * @return The unique integer ID.
     */
    [[nodiscard]] uint32_t get_id() const {
        return id_;
    }

    /**
     * @brief Equality comparison operator.
     * @param [in] other The other ThreadID to compare against.
     * @return `true` if the IDs are equal, `false` otherwise.
     */
    [[nodiscard]] bool operator==(const ThreadID& other) const {
        return id_ == other.id_;
    }

    /**
     * @brief Inequality comparison operator.
     * @param [in] other The other ThreadID to compare against.
     * @return `true` if the IDs are not equal, `false` otherwise.
     */
    [[nodiscard]] bool operator!=(const ThreadID& other) const {
        return id_ != other.id_;
    }

    /**
     * @brief Returns a unique ID for the calling thread.
     * @return A `ThreadID` instance for the current thread.
     */
    [[nodiscard]] static ThreadID this_thread_id() {
        static std::atomic<uint32_t> next_id = 1;
        thread_local uint32_t current_id = next_id.fetch_add(1);
        return ThreadID(current_id);
    }

  private:
    uint32_t id_;
};

/**
 * @brief Overloads the stream insertion operator for `ThreadID`.
 * @param [in,out] os The output stream.
 * @param [in] tid The ThreadID to output.
 * @return A reference to the output stream.
 */
std::ostream& operator<<(std::ostream& os, const ThreadID& tid);

} // namespace pubsub_itc_fw

// Define a hash for ThreadID for use in standard containers
namespace std {
template <>
struct hash<pubsub_itc_fw::ThreadID> {
    [[nodiscard]] std::size_t operator()(const pubsub_itc_fw::ThreadID& tid) const noexcept {
        return hash<uint32_t>()(tid.get_id());
    }
};
} // namespace std

#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.

#include <cstdint>
#include <functional> // For std::hash
#include <iosfwd>     // For std::ostream

namespace pubsub_itc_fw {

/** @ingroup utilities_subsystem */

/**
 * @brief A generic template for creating type-safe ID classes.
 *
 * This class serves as a strong typedef, wrapping an underlying integer type
 * to provide compile-time type safety. It ensures that an ID for one concept
 * (e.g., a ThreadID) cannot be accidentally used where an ID for another
 * concept (e.g., a TimerID) is expected.
 *
 * @tparam Tag A unique, empty struct used to differentiate between ID types.
 * @tparam T The underlying integer type.
 */
template <typename Tag, typename T>
class WrappedInteger  {
public:
    /**
     * @brief Constructs an ID with a default (invalid) value.
     */
    constexpr WrappedInteger() : value_(0) {}

    /**
     * @brief Constructs an ID from an integer value.
     * @param [in] value The integer value of the ID.
     */
    explicit constexpr WrappedInteger(T value) : value_(value) {}

    /**
     * @brief Checks if the ID is valid (non-zero).
     * @return `true` if the ID is valid, `false` otherwise.
     */
    [[nodiscard]] constexpr bool is_valid() const {
        return value_ != 0;
    }

    /**
     * @brief Retrieves the integer value of the ID.
     * @return The unique integer ID.
     */
    [[nodiscard]] constexpr T get_value() const {
        return value_;
    }

    /**
     * @brief Equality comparison operator.
     * @param [in] other The other ID to compare against.
     * @return `true` if the IDs are equal, `false` otherwise.
     */
    [[nodiscard]] constexpr bool operator==(const WrappedInteger& other) const {
        return value_ == other.value_;
    }

    /**
     * @brief Inequality comparison operator.
     * @param [in] other The other ID to compare against.
     * @return `true` if the IDs are not equal, `false` otherwise.
     */
    [[nodiscard]] constexpr bool operator!=(const WrappedInteger& other) const {
        return value_ != other.value_;
    }

    [[nodiscard]] constexpr bool operator<(const WrappedInteger& other) const { return value_ < other.value_; }

    /**
     * @brief Prefix increment operator.
     * @return A reference to this ID after incrementing.
     */
    constexpr WrappedInteger& operator++() {
        ++value_;
        return *this;
    }

    /**
     * @brief Postfix increment operator.
     * @return A copy of the ID before incrementing.
     */
    constexpr WrappedInteger operator++(int) {
        WrappedInteger temp(*this);
        ++value_;
        return temp;
    }

private:
    T value_;
};

/**
 * @brief Overloads the stream insertion operator for the generic ID class.
 *
 * This allows an ID to be printed directly to an output stream, which is useful
 * for logging and debugging.
 *
 * @tparam Tag The tag of the ID type.
 * @tparam T The underlying integer type.
 * @param [in,out] os The output stream.
 * @param [in] id The ID instance to output.
 * @return A reference to the output stream.
 */
template <typename Tag, typename T>
std::ostream& operator<<(std::ostream& os, const WrappedInteger<Tag, T>& id) {
    return os << id.get_value();
}

} // namespace pubsub_itc_fw

// Define a hash for the generic ID class for use in standard containers
namespace std {
template <typename Tag, typename T>
struct hash<pubsub_itc_fw::WrappedInteger<Tag, T>> {
    [[nodiscard]] size_t operator()(const pubsub_itc_fw::WrappedInteger<Tag, T>& id) const {
        return hash<T>()(id.get_value());
    }
};
} // namespace std

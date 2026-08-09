#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

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
 * Neither conversion is implicit. An int cannot become an ID without saying so, and an ID
 * cannot decay back to an int at all -- get_value() is the only way out. That is the point:
 * a plain integer ID passed to the wrong parameter compiles and misbehaves at run time,
 * where this does not compile.
 *
 * ==========================================================================
 * ZERO IS RESERVED -- READ THIS BEFORE ADDING AN ID TYPE
 * ==========================================================================
 *
 * A default-constructed ID holds zero, and is_valid() reports zero as invalid. Zero
 * therefore means **"not yet assigned"**, and no generator may ever issue it.
 *
 * **Any counter minting these IDs must start at 1.** `Reactor` does exactly that --
 * `next_connection_id_{1}`, `next_timer_id_{1}` -- and a new ID type that starts its counter
 * at 0 silently breaks every is_valid() test on it: a real, live ID would report itself
 * unassigned.
 *
 * What the reservation buys is that an ID can be a plain value member with a meaningful
 * default. `TopicPublisher` relies on this in roughly ten places, because a subscriber may
 * hold a control connection and no data connection yet:
 *
 *     if (subscriber.data_connection_id.is_valid() && subscriber.data_connection_id != connection_id) {
 *
 * The alternatives all cost something at every use: a std::optional wrapper, a parallel
 * bool, or a named sentinel constant everyone has to remember. Giving up one value out of
 * the type's range costs nothing, **provided the framework owns the counter**.
 *
 * That proviso is the limit of this class. It suits identifiers handed out by a generator,
 * and does not suit a value with internal structure -- a handle packing an index and a
 * generation, say -- where the all-zero combination is a legitimate value rather than a
 * spare one. Such a type needs its own sentinel and should not be built on this template
 * merely for the type safety; see SlabHandle.hpp for one that is not.
 *
 * @tparam Tag A unique, empty struct used to differentiate between ID types.
 * @tparam T The underlying integer type.
 */
template <typename Tag, typename T> class WrappedInteger {
  public:
    /**
     * @brief Constructs an unassigned ID.
     *
     * Holds zero, which is_valid() reports as invalid. Exists so an ID can be a plain member
     * of a struct that is filled in later; see the reservation note on the class.
     */
    constexpr WrappedInteger() : value_(0) {}

    /**
     * @brief Constructs an ID from an integer value.
     * @param [in] value The integer value of the ID.
     */
    explicit constexpr WrappedInteger(T value) : value_(value) {}

    /**
     * @brief Whether this ID has been assigned.
     *
     * Zero means unassigned, so this answers "has a generator given me a value?" rather than
     * "is this value within range" -- it cannot detect an ID that is well-formed but stale,
     * or one issued by a different generator of the same type.
     *
     * @return `true` if the ID is non-zero.
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

    [[nodiscard]] constexpr bool operator<(const WrappedInteger& other) const {
        return value_ < other.value_;
    }

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
template <typename Tag, typename T> std::ostream& operator<<(std::ostream& os, const WrappedInteger<Tag, T>& id) {
    return os << id.get_value();
}

} // namespaces

// Define a hash for the generic ID class for use in standard containers
namespace std {
template <typename Tag, typename T> struct hash<pubsub_itc_fw::WrappedInteger<Tag, T>> {
    [[nodiscard]] size_t operator()(const pubsub_itc_fw::WrappedInteger<Tag, T>& id) const {
        return hash<T>()(id.get_value());
    }
};
} // namespaces

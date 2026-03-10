#pragma once

#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace pubsub_itc_fw::utils {

/** @ingroup messaging_subsystem */

/**
 * @brief A simple, lightweight span-like wrapper for contiguous sequences
 *
 * This class provides a non-owning view over a contiguous sequence of objects.
 * It's designed to be a drop-in replacement for std::span/boost::span in
 * environments where these are not available.
 *
 * @tparam T The element type
 */
template <typename T> class SimpleSpan {
  public:
    // Type aliases (following std::span conventions)
    using element_type = T;
    using value_type = std::remove_cv_t<T>;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using iterator = T*;
    using const_iterator = const T*;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // Special value for "until the end"
    static constexpr size_type npos = static_cast<size_type>(-1);

  private:
    T* data_;
    size_type size_;

  public:
    // --- Constructors ---

    /**
     * @brief Default constructor - creates an empty span
     */
    constexpr SimpleSpan() : data_(nullptr), size_(0) {}

    /**
     * @brief Construct from pointer and size
     */
    constexpr SimpleSpan(T* data, size_type size) : data_(data), size_(size) {}

    /**
     * @brief Construct from pointer range [begin, end)
     */
    constexpr SimpleSpan(T* begin, T* end) : data_(begin), size_(static_cast<size_type>(end - begin)) {}

    /**
     * @brief Construct from std::vector (non-const)
     */
    template <typename Allocator> SimpleSpan(std::vector<value_type, Allocator>& vec) : data_(vec.data()), size_(vec.size()) {}

    /**
     * @brief Construct from std::vector (const) - only for const T
     */
    template <typename Allocator> SimpleSpan(const std::vector<value_type, Allocator>& vec) : data_(vec.data()), size_(vec.size()) {
        static_assert(std::is_const_v<T>, "Cannot create non-const span from const vector");
    }

    /**
     * @brief Construct from C-style array
     */
    template <size_type N> constexpr SimpleSpan(T (&array)[N]) : data_(array), size_(N) {}

    // Copy constructor and assignment (default is fine)
    SimpleSpan(const SimpleSpan&) = default;
    SimpleSpan& operator=(const SimpleSpan&) = default;

    // --- Element Access ---

    /**
     * @brief Unchecked element access
     */
    constexpr reference operator[](size_type index) const {
        return data_[index];
    }

    /**
     * @brief Bounds-checked element access
     */
    constexpr reference at(size_type index) const {
        if (index >= size_) {
            throw std::out_of_range("SimpleSpan::at: index out of range");
        }
        return data_[index];
    }

    /**
     * @brief Access first element
     */
    constexpr reference front() const {
        return data_[0];
    }

    /**
     * @brief Access last element
     */
    constexpr reference back() const {
        return data_[size_ - 1];
    }

    /**
     * @brief Direct access to underlying data
     */
    constexpr pointer data() const {
        return data_;
    }

    // --- Iterators ---

    constexpr iterator begin() const {
        return data_;
    }
    constexpr iterator end() const {
        return data_ + size_;
    }
    constexpr const_iterator cbegin() const {
        return data_;
    }
    constexpr const_iterator cend() const {
        return data_ + size_;
    }

    constexpr reverse_iterator rbegin() const {
        return reverse_iterator(end());
    }
    constexpr reverse_iterator rend() const {
        return reverse_iterator(begin());
    }
    constexpr const_reverse_iterator crbegin() const {
        return const_reverse_iterator(cend());
    }
    constexpr const_reverse_iterator crend() const {
        return const_reverse_iterator(cbegin());
    }

    // --- Capacity ---

    /**
     * @brief Number of elements in the span
     */
    constexpr size_type size() const {
        return size_;
    }

    /**
     * @brief Size in bytes
     */
    constexpr size_type size_bytes() const {
        return size_ * sizeof(T);
    }

    /**
     * @brief Check if span is empty
     */
    constexpr bool empty() const {
        return size_ == 0;
    }

    // --- Subviews ---

    /**
     * @brief Create a subspan starting at offset
     * @param offset Starting position
     * @param count Number of elements (npos means until the end)
     */
    constexpr SimpleSpan subspan(size_type offset, size_type count = npos) const {
        if (offset > size_) {
            throw std::out_of_range("SimpleSpan::subspan: offset out of range");
        }

        size_type actual_count = (count == npos) ? (size_ - offset) : count;
        if (offset + actual_count > size_) {
            throw std::out_of_range("SimpleSpan::subspan: count too large");
        }

        return SimpleSpan(data_ + offset, actual_count);
    }

    /**
     * @brief Get first n elements
     */
    constexpr SimpleSpan first(size_type count) const {
        if (count > size_) {
            throw std::out_of_range("SimpleSpan::first: count too large");
        }
        return SimpleSpan(data_, count);
    }

    /**
     * @brief Get last n elements
     */
    constexpr SimpleSpan last(size_type count) const {
        if (count > size_) {
            throw std::out_of_range("SimpleSpan::last: count too large");
        }
        return SimpleSpan(data_ + size_ - count, count);
    }
};

// --- Deduction guides (C++17) ---
template <typename T, size_t N> SimpleSpan(T (&)[N]) -> SimpleSpan<T>;

template <typename T, typename Allocator> SimpleSpan(std::vector<T, Allocator>&) -> SimpleSpan<T>;

template <typename T, typename Allocator> SimpleSpan(const std::vector<T, Allocator>&) -> SimpleSpan<const T>;

// --- Comparison operators ---
template <typename T, typename U> constexpr bool operator==(const SimpleSpan<T>& lhs, const SimpleSpan<U>& rhs) {
    if (lhs.size() != rhs.size())
        return false;
    return std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

template <typename T, typename U> constexpr bool operator!=(const SimpleSpan<T>& lhs, const SimpleSpan<U>& rhs) {
    return !(lhs == rhs);
}

template <typename T, typename U> constexpr bool operator<(const SimpleSpan<T>& lhs, const SimpleSpan<U>& rhs) {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

template <typename T, typename U> constexpr bool operator<=(const SimpleSpan<T>& lhs, const SimpleSpan<U>& rhs) {
    return !(rhs < lhs);
}

template <typename T, typename U> constexpr bool operator>(const SimpleSpan<T>& lhs, const SimpleSpan<U>& rhs) {
    return rhs < lhs;
}

template <typename T, typename U> constexpr bool operator>=(const SimpleSpan<T>& lhs, const SimpleSpan<U>& rhs) {
    return !(lhs < rhs);
}

} // namespace pubsub_itc_fw::utils

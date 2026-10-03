#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstddef>
#include <new>
#include <utility>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

/**
 * @file FixedCapacityRingBuffer.hpp
 * @brief A first-in, first-out container whose capacity is fixed when it is created.
 *
 * Elements are added at the back with push_back or emplace_back and removed from the front with
 * pop_front, so they leave in the order they arrived. The storage is a ring: when a position
 * reaches the end of the storage it continues from the start.
 *
 * **Adding to a full buffer is refused.** The call returns false and the buffer is unchanged.
 * Many ring buffers, including Boost's circular_buffer, instead overwrite the oldest element when
 * full. A caller that wants that can remove the oldest element first and then add; a buffer that
 * overwrites by itself cannot be made to refuse, and some callers need the refusal. A rate
 * throttle, for example, holds the times of the commands it accepted in the last second, and a
 * full buffer is exactly the answer that the limit has been reached.
 *
 * **Memory.** Storage for exactly the capacity is taken once, in the constructor, from the global
 * operator new, and is never resized. Adding and removing elements never use the heap. The aligned
 * form of operator new is used, with the element type's own alignment, because the plain form
 * promises no more than the alignment of max_align_t and an element type may need more. Elements
 * are constructed in place when they are added and destroyed when they are removed, or when the
 * buffer is destroyed while still holding them. So the element type does not
 * need a default constructor, and no element exists in a slot that is not in use. The storage is
 * reported to an optional AllocationGrowthReporter, as the framework's other containers report
 * theirs, so a component can account for the memory its buffers hold.
 *
 * **How the positions are kept.** The buffer records the index of the oldest element and the
 * number of elements held, and works out the index of the next free slot from those two. Keeping
 * separate read and write indexes instead would leave an empty buffer and a full one looking the
 * same, because in both cases the two indexes are equal.
 *
 * **Misuse** is refused with PreconditionAssertion: a capacity of zero, and asking for or removing
 * the front element of an empty buffer. A full buffer refusing an element is not misuse, and is
 * reported by push_back and emplace_back returning false.
 *
 * **Copying** is not supported, because a copy would have to allocate, and this class otherwise
 * allocates only when it is created. Moving is supported: the storage passes to the new owner and
 * the buffer moved from is left with no storage and a capacity of zero, fit only to be destroyed
 * or assigned to.
 *
 * **Threading.** Not thread-safe. Each buffer must be used by one thread, or every call protected
 * by the caller.
 *
 * **Why a template.** The buffer is a general-purpose framework container, so the type of element
 * it holds is the user's choice, not the buffer's. Its first user, the gateways' rate throttle,
 * holds std::chrono::steady_clock::time_point values; another might hold sequence numbers,
 * pointers or small structs, and gets the same refusal when full. As a template, each element
 * type gets its own compiled copy, so an element is stored and read with no conversion, no
 * virtual call and no allocation per element.
 *
 * @tparam T The element type. It need not have a default constructor; it must be movable or
 *           copyable into the buffer, and its destructor must not throw.
 */
template <typename T> class FixedCapacityRingBuffer {
  public:
    ~FixedCapacityRingBuffer() {
        release_storage();
    }

    /**
     * @brief Creates an empty buffer with room for exactly @p capacity elements.
     *
     * This is the only place the buffer allocates.
     *
     * @param[in] capacity         The most elements the buffer can hold. Must be greater than zero.
     * @param[in] growth_reporter  Non-owning, may be nullptr. Told the size of the storage taken,
     *                             if it is at least the reporter's threshold.
     */
    explicit FixedCapacityRingBuffer(size_t capacity, AllocationGrowthReporter* growth_reporter = nullptr) : capacity_(capacity) {
        if (capacity == 0) {
            throw PreconditionAssertion("FixedCapacityRingBuffer: the capacity must be greater than zero", __FILE__, __LINE__);
        }
        const size_t bytes = capacity * sizeof(T);
        slots_ = static_cast<T*>(::operator new(bytes, std::align_val_t{alignof(T)}));
        report_allocation(growth_reporter, bytes);
    }

    FixedCapacityRingBuffer(const FixedCapacityRingBuffer& other) = delete;
    FixedCapacityRingBuffer& operator=(const FixedCapacityRingBuffer& other) = delete;

    FixedCapacityRingBuffer(FixedCapacityRingBuffer&& other)
        : slots_(std::exchange(other.slots_, nullptr))
        , capacity_(std::exchange(other.capacity_, 0))
        , front_(std::exchange(other.front_, 0))
        , size_(std::exchange(other.size_, 0)) {}

    FixedCapacityRingBuffer& operator=(FixedCapacityRingBuffer&& other) {
        if (this != &other) {
            release_storage();
            slots_ = std::exchange(other.slots_, nullptr);
            capacity_ = std::exchange(other.capacity_, 0);
            front_ = std::exchange(other.front_, 0);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    /**
     * @brief Adds a copy of @p value at the back.
     * @param[in] value The element to add.
     * @return True if it was added; false if the buffer was full, in which case nothing changed.
     */
    [[nodiscard]] bool push_back(const T& value) {
        return emplace_back(value);
    }

    /**
     * @brief Adds @p value at the back, moving it in.
     * @param[in] value The element to add. Left as it was if the buffer was full.
     * @return True if it was added; false if the buffer was full, in which case nothing changed.
     */
    [[nodiscard]] bool push_back(T&& value) {
        return emplace_back(std::move(value));
    }

    /**
     * @brief Constructs an element at the back, in place, from @p arguments.
     * @param[in] arguments Passed to the element's constructor.
     * @return True if it was added; false if the buffer was full, in which case nothing was
     *         constructed and nothing changed.
     */
    template <typename... Arguments> [[nodiscard]] bool emplace_back(Arguments&&... arguments) {
        if (full()) {
            return false;
        }
        // The element is constructed before the count is raised, so an element whose constructor
        // throws leaves the buffer exactly as it was.
        ::new (static_cast<void*>(slots_ + index_after_front(size_))) T(std::forward<Arguments>(arguments)...);
        ++size_;
        return true;
    }

    /**
     * @brief Removes the oldest element and destroys it.
     *
     * The buffer must not be empty.
     */
    void pop_front() {
        if (empty()) {
            throw PreconditionAssertion("FixedCapacityRingBuffer: pop_front called on an empty buffer", __FILE__, __LINE__);
        }
        slots_[front_].~T();
        front_ = index_after_front(1);
        --size_;
    }

    /**
     * @brief The oldest element, which is the next to be removed.
     *
     * The buffer must not be empty.
     * @return A reference that stays valid until the element is removed.
     */
    [[nodiscard]] T& front() {
        if (empty()) {
            throw PreconditionAssertion("FixedCapacityRingBuffer: front called on an empty buffer", __FILE__, __LINE__);
        }
        return slots_[front_];
    }

    /**
     * @brief The oldest element, which is the next to be removed.
     *
     * The buffer must not be empty.
     * @return A reference that stays valid until the element is removed.
     */
    [[nodiscard]] const T& front() const {
        if (empty()) {
            throw PreconditionAssertion("FixedCapacityRingBuffer: front called on an empty buffer", __FILE__, __LINE__);
        }
        return slots_[front_];
    }

    /// The number of elements held.
    [[nodiscard]] size_t size() const {
        return size_;
    }

    /// The most elements the buffer can hold.
    [[nodiscard]] size_t capacity() const {
        return capacity_;
    }

    /// True if the buffer holds no elements.
    [[nodiscard]] bool empty() const {
        return size_ == 0;
    }

    /// True if the buffer holds as many elements as its capacity, so that adding one would be refused.
    [[nodiscard]] bool full() const {
        return size_ == capacity_;
    }

    /// The number of bytes of storage the buffer took when it was created.
    [[nodiscard]] size_t allocated_bytes() const {
        return capacity_ * sizeof(T);
    }

  private:
    /// The index of the slot @p offset places after the front, continuing from the start of the
    /// storage past its end. @p offset is never more than the capacity, so one subtraction is enough.
    [[nodiscard]] size_t index_after_front(size_t offset) const {
        size_t index = front_ + offset;
        if (index >= capacity_) {
            index -= capacity_;
        }
        return index;
    }

    /// Destroys every element still held, oldest first, and returns the storage.
    void release_storage() {
        if (slots_ == nullptr) {
            return;
        }
        while (size_ > 0) {
            slots_[front_].~T();
            front_ = index_after_front(1);
            --size_;
        }
        ::operator delete(slots_, std::align_val_t{alignof(T)});
        slots_ = nullptr;
    }

    /// Tells the reporter how large the storage is, as IncrementalRehashMap reports its tables.
    static void report_allocation(AllocationGrowthReporter* growth_reporter, size_t bytes) {
        if (growth_reporter == nullptr || bytes < growth_reporter->report_threshold_bytes) {
            return;
        }
        size_t previous = growth_reporter->largest_allocation_bytes.load(std::memory_order_relaxed);
        while (bytes > previous && !growth_reporter->largest_allocation_bytes.compare_exchange_weak(previous, bytes, std::memory_order_relaxed)) {}
        if (growth_reporter->on_large_allocation) {
            growth_reporter->on_large_allocation(bytes, growth_reporter->largest_allocation_bytes.load(std::memory_order_relaxed));
        }
    }

    T* slots_{nullptr};
    size_t capacity_{0};
    // The index of the oldest element. Meaningful only when size_ is greater than zero.
    size_t front_{0};
    size_t size_{0};
};

} // namespaces

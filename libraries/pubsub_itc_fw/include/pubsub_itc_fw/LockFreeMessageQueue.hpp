#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <functional>
#include <optional>
#include <type_traits>
#include <cstddef>

/** @ingroup queue_subsystem */

#ifdef USING_VALGRIND
#include <mutex>
#include <deque>
#endif

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ExpandablePoolAllocator.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>

namespace pubsub_itc_fw {

#ifdef USING_VALGRIND
/**
 * @brief Valgrind-compatible MPSC queue using mutex.
 *
 * When USING_VALGRIND is defined, we use a simple mutex-protected std::deque
 * instead of lock-free atomics. This allows Valgrind's tools (Helgrind, DRD)
 * to properly analyze the code without getting confused by lock-free algorithms.
 */
template<typename T>
class LockFreeMessageQueue {
    static_assert(std::is_move_constructible_v<T>,
                  "LockFreeMessageQueue requires a move-constructible element type.");

public:
    LockFreeMessageQueue(const LockFreeMessageQueue&) = delete;
    LockFreeMessageQueue& operator=(const LockFreeMessageQueue&) = delete;
    LockFreeMessageQueue(LockFreeMessageQueue&&) = delete;
    LockFreeMessageQueue& operator=(LockFreeMessageQueue&&) = delete;

    LockFreeMessageQueue(const QueueConfig& queue_config,
                         const AllocatorConfig& allocator_config)
        : queue_configuration_(queue_config)
        , allocator_configuration_(allocator_config)
    {
    }

    ~LockFreeMessageQueue() {
        shutdown();
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
        queue_.clear();
    }

    template<typename... Args>
    void enqueue(Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (shutting_down_) {
            return;
        }

        queue_.emplace_back(std::forward<Args>(args)...);

        int current_size = static_cast<int>(queue_.size());
        if (queue_configuration_.gone_above_high_watermark_handler &&
            current_size >= queue_configuration_.high_watermark &&
            !is_high_watermark_breached_)
        {
            is_high_watermark_breached_ = true;
            queue_configuration_.gone_above_high_watermark_handler(queue_configuration_.for_client_use);
        }
    }

    [[nodiscard]] std::optional<T> dequeue() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            return std::nullopt;
        }

        T value = std::move(queue_.front());
        queue_.pop_front();

        int current_size = static_cast<int>(queue_.size());
        if (queue_configuration_.gone_below_low_watermark_handler &&
            current_size < queue_configuration_.low_watermark &&
            is_high_watermark_breached_)
        {
            is_high_watermark_breached_ = false;
            queue_configuration_.gone_below_low_watermark_handler(queue_configuration_.for_client_use);
        }

        return value;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::deque<T> queue_;
    QueueConfig queue_configuration_;
    AllocatorConfig allocator_configuration_;
    bool shutting_down_{false};
    bool is_high_watermark_breached_{false};
};

#else // !USING_VALGRIND

/**
 * @brief Lock-free MPSC queue using Vyukov's algorithm.
 *
 * This queue is owned by a single consumer thread and may have multiple
 * producers. Nodes are allocated from an ExpandablePoolAllocator<Node>.
 *
 * @tparam T Must be move-constructible.
 */
template<typename T>
class LockFreeMessageQueue {
    static_assert(std::is_move_constructible_v<T>,
                  "LockFreeMessageQueue requires a move-constructible element type.");

private:
    struct Node {
        std::atomic<Node*> next_;
        alignas(T) std::byte data_storage_[sizeof(T)];
        bool has_data_;

        Node() : next_(nullptr), has_data_(false) {}

        ~Node() {
            if (has_data_) {
                reinterpret_cast<T*>(data_storage_)->~T();
            }
        }

        T& data() {
            return *reinterpret_cast<T*>(data_storage_);
        }
    };

public:
    ~LockFreeMessageQueue() {
        shutdown();
    }

    LockFreeMessageQueue(const LockFreeMessageQueue&) = delete;
    LockFreeMessageQueue& operator=(const LockFreeMessageQueue&) = delete;
    LockFreeMessageQueue(LockFreeMessageQueue&&) = delete;
    LockFreeMessageQueue& operator=(LockFreeMessageQueue&&) = delete;

    /**
     * @brief Constructs a queue using the provided configuration objects.
     */
    LockFreeMessageQueue(const QueueConfiguration& queue_config,
                         const AllocatorConfiguration& allocator_config)
        : stub_()
        , head_(&stub_)
        , tail_(&stub_)
        , queue_configuration_(queue_config)
        , allocator_configuration_(allocator_config)
        , node_allocator_(allocator_configuration_.pool_name,
                          allocator_configuration_.objects_per_pool,
                          allocator_configuration_.initial_pools,
                          allocator_configuration_.expansion_threshold_hint,
                          allocator_configuration_.handler_for_pool_exhausted,
                          allocator_configuration_.handler_for_invalid_free,
                          allocator_configuration_.handler_for_huge_pages_error,
                          allocator_configuration_.use_huge_pages_flag)
        , size_{0}
    {
        stub_.next_.store(nullptr, std::memory_order_relaxed);
    }

    /**
     * @brief Initiates shutdown and drains all remaining messages.
     */
    void shutdown() {
        bool expected = false;
        if (!shutting_down_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;
        }

        // After this point, enqueue() becomes a no-op and producers
        // will silently drop messages. The single consumer thread
        // remains responsible for draining any remaining items via
        // dequeue(), preserving the MPSC contract.
    }

    /**
     * @brief Enqueues an element. Never fails unless shutting down.
     */
    template<typename... Args>
    void enqueue(Args&&... args) {
        if (shutting_down_.load(std::memory_order_acquire)) {
            return;
        }

        Node* node = node_allocator_.allocate();  // guaranteed non-null

        node->has_data_ = true;
        new (node->data_storage_) T(std::forward<Args>(args)...);
        node->next_.store(nullptr, std::memory_order_relaxed);

        Node* prev_head = head_.exchange(node, std::memory_order_acq_rel);

        // This is just used by a unit test to explore priority inversion.
        if (test_stall_callback_) {
            test_stall_callback_();
        }

        prev_head->next_.store(node, std::memory_order_release);

        int current_size = ++size_;
        if (queue_configuration_.gone_above_high_watermark_handler &&
            current_size >= queue_configuration_.high_watermark &&
            !is_high_watermark_breached_.exchange(true))
        {
            queue_configuration_.gone_above_high_watermark_handler(queue_configuration_.for_client_use);
        }
    }

    /**
     * @brief Dequeues an element. Only the consumer thread may call this.
     */
    [[nodiscard]] std::optional<T> dequeue() {
        Node* tail = tail_;
        Node* next = tail->next_.load(std::memory_order_acquire);

        if (tail == &stub_) {
            if (next == nullptr) {
                return std::nullopt;
            }
            tail_ = next;
            tail = next;
            next = next->next_.load(std::memory_order_acquire);
        }

        if (next != nullptr) {
            std::optional<T> result{std::move(tail->data())};
            tail->has_data_ = false;
            tail_ = next;
            node_allocator_.deallocate(tail);

            int current_size = --size_;
            if (queue_configuration_.gone_below_low_watermark_handler &&
                current_size < queue_configuration_.low_watermark &&
                is_high_watermark_breached_.exchange(false))
            {
                queue_configuration_.gone_below_low_watermark_handler(queue_configuration_.for_client_use);
            }
            return result;
        }

        Node* head = head_.load(std::memory_order_acquire);
        if (tail != head) {
            return std::nullopt;
        }

        enqueue_stub_();
        next = tail->next_.load(std::memory_order_acquire);

        if (next != nullptr) {
            std::optional<T> result{std::move(tail->data())};
            tail->has_data_ = false;
            tail_ = next;
            node_allocator_.deallocate(tail);

            int current_size = --size_;
            if (queue_configuration_.gone_below_low_watermark_handler &&
                current_size < queue_configuration_.low_watermark &&
                is_high_watermark_breached_.exchange(false))
            {
                queue_configuration_.gone_below_low_watermark_handler(queue_configuration_.for_client_use);
            }
            return result;
        }

        return std::nullopt;
    }

    /**
     * @brief Checks if the queue is empty.
     */
    bool empty() const {
        Node* tail = tail_;
        if (tail == &stub_) {
            return tail->next_.load(std::memory_order_acquire) == nullptr;
        }
        return tail->next_.load(std::memory_order_acquire) == nullptr &&
               tail == head_.load(std::memory_order_acquire);
    }

    // To test for priority inversion we need to add some hooks that are just used by a unit test.
    std::function<void()> test_stall_callback_;

private:
    void enqueue_stub_() {
        stub_.next_.store(nullptr, std::memory_order_relaxed);
        Node* prev_head = head_.exchange(&stub_, std::memory_order_acq_rel);
        prev_head->next_.store(&stub_, std::memory_order_release);
    }

    static constexpr size_t cache_line_size_ = 64;

    /**
     * @brief Permanent dummy node required by the Vyukov MPSC queue algorithm.
     *
     * The queue is implemented as a singly-linked list with multiple producers
     * and a single consumer. A permanent dummy node (the "stub") anchors the list
     * so that the queue is never structurally empty:
     *
     *     head_ --> stub_ --> first real node --> ...
     *     tail_ ------------------------------------^
     *
     * Invariants provided by the stub:
     *   - head_ and tail_ always point to valid nodes; neither is ever nullptr.
     *   - Producers always append to a well-formed list without needing empty-queue
     *     special cases.
     *   - The consumer always pops from stub_.next_, never from head_ itself.
     *   - The stub is never allocated from, or returned to, the memory pool; its
     *     address is stable for the lifetime of the queue.
     *
     * These invariants eliminate empty/non-empty transition races and simplify the
     * lock-free enqueue/dequeue logic. The stub must be constructed before head_
     * and tail_ are initialised.
     */
    Node stub_;

    alignas(cache_line_size_) std::atomic<Node*> head_;
    alignas(cache_line_size_) Node* tail_;

    QueueConfiguration queue_configuration_;
    AllocatorConfiguration allocator_configuration_;
    ExpandablePoolAllocator<Node> node_allocator_;

    std::atomic<int> size_{0};
    std::atomic<bool> is_high_watermark_breached_{false};
    std::atomic<bool> shutting_down_{false};
};

#endif // check for is or is not USING_VALGRIND

} // namespace pubsub_itc_fw

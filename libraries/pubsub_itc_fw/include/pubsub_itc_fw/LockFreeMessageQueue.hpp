#pragma once

#include <atomic>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>

namespace pubsub_itc_fw {

/**
 * @brief Provides a lock-free queue implementation for a single consumer and multiple producers.
 *
 * This implementation is based on Dmitry Vyukov's algorithm and is optimized for
 * high-performance, inter-thread communication. It offers wait-free enqueue
 * and lock-free dequeue operations.
 *
 * @tparam T The type of elements to store in the queue. Must be move-constructible.
 */
template<typename T>
class LockFreeMessageQueue {
    static_assert(std::is_move_constructible_v<T>, "LockFreeMessageQueue requires a move-constructible element type.");

private:
    // Node structure for the intrusive linked list.
    // Made a private inner struct as it is an implementation detail.
    struct Node {
        std::atomic<Node*> next_{nullptr};
        alignas(T) std::byte data_storage_[sizeof(T)];
        bool has_data_{false};

        // Constructor for a data node.
        template<typename... Args>
        explicit Node(Args&&... args) : has_data_(true) {
            new(data_storage_) T(std::forward<Args>(args)...);
        }

        // Constructor for a stub node.
        Node() : has_data_(false) {}

        // Destructor to handle data destruction.
        ~Node() {
            if (has_data_) {
                reinterpret_cast<T*>(data_storage_)->~T();
            }
        }

        // Accessor for data.
        T& data() { return *reinterpret_cast<T*>(data_storage_); }
    };

public:
    // Non-copyable, non-movable for safety and simplicity.
    LockFreeMessageQueue(const LockFreeMessageQueue&) = delete;
    LockFreeMessageQueue& operator=(const LockFreeMessageQueue&) = delete;
    LockFreeMessageQueue(LockFreeMessageQueue&&) = delete;
    LockFreeMessageQueue& operator=(LockFreeMessageQueue&&) = delete;

    /**
     * @brief Constructs a LockFreeMessageQueue with watermark support.
     * @param low_watermark The low watermark for the queue size.
     * @param high_watermark The high watermark for the queue size.
     * @param for_client_use A client-provided pointer for handler use.
     * @param gone_below_low_watermark_handler The handler to call when the queue size drops below the low watermark.
     * @param gone_above_high_watermark_handler The handler to call when the queue size exceeds the high watermark.
     */
    LockFreeMessageQueue(int low_watermark,
                         int high_watermark,
                         void* for_client_use,
                         std::function<void(void* for_client_use)> gone_below_low_watermark_handler,
                         std::function<void(void* for_client_use)> gone_above_high_watermark_handler)
        : head_{&stub_},
          tail_{&stub_},
          size_{0},
          low_watermark_{low_watermark},
          high_watermark_{high_watermark},
          for_client_use_{for_client_use},
          gone_below_low_watermark_handler_{std::move(gone_below_low_watermark_handler)},
          gone_above_high_watermark_handler_{std::move(gone_above_high_watermark_handler)} {
        stub_.next_.store(nullptr, std::memory_order_relaxed);
    }

    /**
     * @brief Constructs an empty queue.
     *
     * Initializes the queue with a stub node to simplify the algorithm
     * and avoid special-casing an empty queue.
     */
    LockFreeMessageQueue() : LockFreeMessageQueue(0, 0, nullptr, nullptr, nullptr) {}

    /**
     * @brief Destructs the queue and drains all remaining elements.
     *
     * This method ensures all allocated nodes are properly deallocated to prevent
     * memory leaks.
     */
    ~LockFreeMessageQueue() {
        while (!empty()) {
            dequeue();
        }
    }

    /**
     * @brief Enqueues an element.
     *
     * This operation is thread-safe and can be called by multiple producers
     * concurrently without contention.
     *
     * @tparam Args The types of the arguments to construct the element.
     * @param [in] args The arguments to forward to the element's constructor.
     */
    template<typename... Args>
    void enqueue(Args&&... args) {
        auto* node = new(std::nothrow) Node(std::forward<Args>(args)...);
        if (node == nullptr) {
            // TODO handle this gracefully.
            return;
        }

        node->next_.store(nullptr, std::memory_order_relaxed);
        Node* prev_head = head_.exchange(node, std::memory_order_acq_rel);
        prev_head->next_.store(node, std::memory_order_release);
        int current_size = ++size_;
        if (current_size >= high_watermark_ && !is_high_watermark_breached_.exchange(true)) {
            gone_above_high_watermark_handler_(for_client_use_);
        }
    }

    /**
     * @brief Attempts to dequeue an element.
     *
     * This operation is lock-free and should only be called by a single consumer
     * thread.
     *
     * @returns std::optional<T> The dequeued element, or std::nullopt if the queue is empty.
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
            tail_ = next;
            delete tail;
            int current_size = --size_;
            if (current_size < low_watermark_ && is_high_watermark_breached_.exchange(false)) {
                gone_below_low_watermark_handler_(for_client_use_);
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
            tail_ = next;
            delete tail;
            int current_size = --size_;
            if (current_size < low_watermark_ && is_high_watermark_breached_.exchange(false)) {
                gone_below_low_watermark_handler_(for_client_use_);
            }
            return result;
        }

        return std::nullopt;
    }

    /**
     * @brief Checks if the queue is empty.
     *
     * This is a snapshot and may not reflect concurrent enqueue operations.
     *
     * @returns bool True if the queue appears empty, false otherwise.
     */
    bool empty() const {
        Node* tail = tail_;
        if (tail == &stub_) {
            return tail->next_.load(std::memory_order_acquire) == nullptr;
        }
        return tail->next_.load(std::memory_order_acquire) == nullptr &&
               tail == head_.load(std::memory_order_acquire);
    }

private:
    /**
     * @brief Pushes a stub node to maintain queue invariants.
     */
    void enqueue_stub_() {
        stub_.next_.store(nullptr, std::memory_order_relaxed);
        Node* prev_head = head_.exchange(&stub_, std::memory_order_acq_rel);
        prev_head->next_.store(&stub_, std::memory_order_release);
    }

    // Aligned to cache line size to prevent false sharing.
    static constexpr size_t cache_line_size_ = 64;

    // Head pointer for producers.
    alignas(cache_line_size_) std::atomic<Node*> head_;

    // Tail pointer for the single consumer.
    alignas(cache_line_size_) Node* tail_;

    // Stub node to handle the empty queue case.
    Node stub_;

    std::atomic<int> size_{0};
    int low_watermark_;
    int high_watermark_;

    void* for_client_use_;
    std::function<void(void* for_client_use)> gone_below_low_watermark_handler_;
    std::function<void(void* for_client_use)> gone_above_high_watermark_handler_;

    std::atomic<bool> is_high_watermark_breached_{false};
};

} // namespace pubsub_itc_fw

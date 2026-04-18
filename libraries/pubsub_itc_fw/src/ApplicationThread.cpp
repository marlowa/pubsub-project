// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/BackoffWithYield.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/HighResolutionClock.hpp>
#include <pubsub_itc_fw/LockFreeMessageQueue.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadLifecycleState.hpp>
#include <pubsub_itc_fw/TimerID.hpp>
#include <pubsub_itc_fw/TimerType.hpp>

namespace pubsub_itc_fw {

ApplicationThread::~ApplicationThread() {
    // The Reactor's finalize_threads_after_shutdown() is responsible for joining
    // all threads before their shared_ptrs are released. If this destructor is
    // reached with a joinable thread, it means either:
    //   (a) finalize_threads_after_shutdown() was not called -- a programming error, or
    //   (b) the thread refused to join within the shutdown timeout -- an unrecoverable
    //       condition. Detaching is not safe because the thread is still running and
    //       still holds a reference to this object. std::terminate() is the only
    //       honest response.
    if (thread_ != nullptr && thread_->joinable()) {
        PUBSUB_LOG(logger_, FwLogLevel::Error,
            "ApplicationThread {} destroyed while thread is still joinable. "
            "This indicates finalize_threads_after_shutdown() was not called "
            "or the thread refused to stop. Terminating.", thread_name_);
        std::terminate();
    }

    // Note: We do not tell the reactor to deregister the thread.
    // The reactor owns the threads.
}

ApplicationThread::ApplicationThread(QuillLogger& logger, Reactor& reactor, const std::string& thread_name, ThreadID thread_id, const QueueConfiguration& queue_config,
                                     const AllocatorConfiguration& allocator_config, const ApplicationThreadConfiguration& thread_config)
    : logger_(logger)
    , reactor_(reactor)
    , outbound_allocator_(thread_config.outbound_slab_size)
    , decode_arena_buffer_()
    , time_event_started_()
    , time_event_finished_()
    , thread_name_(thread_name)
    , thread_id_(thread_id)
    , thread_(nullptr) {
    if (thread_id.get_value() == 0) {
        throw PreconditionAssertion("ThreadID of zero is reserved for the reactor", __FILE__, __LINE__);
    }

    decode_arena_buffer_.reserve(thread_config.inbound_decode_arena_size);
    message_queue_ = std::make_unique<LockFreeMessageQueue<EventMessage>>(queue_config, allocator_config);
    set_lifecycle_state(ThreadLifecycleState::Created);
}

const std::string& ApplicationThread::get_thread_name() const {
    return thread_name_;
}

void ApplicationThread::start() {
    if (thread_ != nullptr) {
        throw PreconditionAssertion(fmt::format("Thread {} has already been started.", thread_name_), __FILE__, __LINE__);
    }

    thread_ = std::make_unique<ThreadWithJoinTimeout>();
    thread_->start([this]() { run(); });

    // Wait for the thread to enter its run loop, but with a bounded number of iterations.
    BackoffWithYield backoff;

    // Instrumentation-aware iteration bound.
    // These values are chosen to be:
    // - deterministic
    // - extremely fast in normal builds
    // - generous enough under TSAN/Valgrind
    constexpr int max_iterations =
    #if defined(USING_TSAN)
        200000;   // TSAN is slow
    #elif defined(USING_VALGRIND)
        500000;   // Valgrind is slower
    #else
        20000;    // normal builds
    #endif

    int iterations = 0;

    while (get_lifecycle_state().as_tag() < ThreadLifecycleState::Started) {
        if (++iterations > max_iterations) {
            throw PubSubItcException(fmt::format("Thread {} failed to reach Started state (startup timeout)", thread_name_));
        }
        backoff.pause();
    }
}

[[nodiscard]] bool ApplicationThread::join_with_timeout(std::chrono::milliseconds timeout) {
    if (thread_ == nullptr) {
        return false;
    }
    return thread_->join_with_timeout(timeout);
}

void ApplicationThread::pause() {
    is_paused_.store(true, std::memory_order_relaxed);
}

void ApplicationThread::resume() {
    is_paused_.store(false, std::memory_order_relaxed);
}

void ApplicationThread::post_message(ThreadID target_thread_id, EventMessage message) {
    if (target_thread_id == thread_id_) {
        // Direct self-post
        message_queue_->enqueue(std::move(message));
        return;
    }

    reactor_.route_message(target_thread_id, std::move(message));
}

TimerID ApplicationThread::start_one_off_timer(const std::string& name, std::chrono::microseconds interval) {
    return schedule_timer(name, interval, TimerType::SingleShot);
}

TimerID ApplicationThread::start_recurring_timer(const std::string& name, std::chrono::microseconds interval) {
    return schedule_timer(name, interval, TimerType::Recurring);
}

void ApplicationThread::cancel_timer(const std::string& name) {
    assert_called_from_owner();

    auto it = name_to_id_.find(name);
    if (it == name_to_id_.end()) {
        return;
    }

    const TimerID id = it->second;
    PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {} sending cancel timer command to reactor for timer {}", thread_name_, name);
    ReactorControlCommand command(ReactorControlCommand::CommandTag::CancelTimer);
    command.owner_thread_id_ = thread_id_;
    command.timer_id_ = id;
    reactor_.enqueue_control_command(command);

    id_to_name_.erase(id);
    name_to_id_.erase(it);
}

void ApplicationThread::connect_to_service(const std::string& service_name)
{
    assert_called_from_owner();

    ReactorControlCommand command(ReactorControlCommand::CommandTag::Connect);
    command.requesting_thread_id_ = thread_id_;
    command.service_name_ = service_name;
    reactor_.enqueue_control_command(command);
}

void ApplicationThread::commit_raw_bytes(ConnectionID conn_id, int64_t bytes_consumed)
{
    if (active_connection_ids_.find(conn_id) == active_connection_ids_.end()) {
        throw PreconditionAssertion(
            fmt::format("ApplicationThread::commit_raw_bytes: ConnectionID {} "
                        "does not belong to this thread", conn_id.get_value()),
            __FILE__, __LINE__);
    }
    ReactorControlCommand command(ReactorControlCommand::CommandTag::CommitRawBytes);
    command.connection_id_  = conn_id;
    command.bytes_consumed_ = bytes_consumed;
    reactor_.enqueue_control_command(command);
}

void ApplicationThread::send_raw(ConnectionID conn_id, const void* data, uint32_t size)
{
    if (active_connection_ids_.find(conn_id) == active_connection_ids_.end()) {
        throw PreconditionAssertion(
            fmt::format("ApplicationThread::send_raw: ConnectionID {} "
                        "does not belong to this thread", conn_id.get_value()),
            __FILE__, __LINE__);
    }

    if (data == nullptr) {
        throw PreconditionAssertion(
            "ApplicationThread::send_raw: data must not be nullptr", __FILE__, __LINE__);
    }
    if (size == 0) {
        throw PreconditionAssertion(
            "ApplicationThread::send_raw: size must be greater than zero", __FILE__, __LINE__);
    }

    auto [slab_id, chunk] = outbound_allocator_.allocate(size);
    std::memcpy(chunk, data, size);

    ReactorControlCommand cmd(ReactorControlCommand::CommandTag::SendRaw);
    cmd.connection_id_  = conn_id;
    cmd.allocator_      = &outbound_allocator_;
    cmd.slab_id_        = slab_id;
    cmd.raw_chunk_ptr_  = chunk;
    cmd.raw_byte_count_ = size;
    reactor_.enqueue_control_command(cmd);
}

// Note: reason is used in the logging macros but we have to neutralise those macros for clang-tidy
void ApplicationThread::shutdown([[maybe_unused]] const std::string& reason) {
    auto state = get_lifecycle_state().as_tag();
    if (state >= ThreadLifecycleState::ShuttingDown) {
        // Already shutting down or terminated; ensure queue is shut down and return.
        if (message_queue_ != nullptr) {
            message_queue_->shutdown();
        }
        return;
    }

    set_lifecycle_state(ThreadLifecycleState::ShuttingDown);

    if (message_queue_ != nullptr) {
        message_queue_->shutdown();
    }

    PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {} received shutdown signal: {}", thread_name_, reason);

    // Joining is the sole responsibility of Reactor::finalize_threads_after_shutdown().
    // ApplicationThread::shutdown() only requests shutdown and makes the queue stop accepting messages.
}

void ApplicationThread::run() {
    try {
        run_internal();
    } catch (const std::exception& ex) {
        PUBSUB_LOG(logger_, FwLogLevel::Error, "{} [{}] terminating due to exception: {}", thread_name_, thread_id_.get_value(), ex.what());
        set_lifecycle_state(ThreadLifecycleState::ShuttingDown);
        reactor_.shutdown(fmt::format("Thread {} [{}] terminated due to exception: {}", thread_name_, thread_id_.get_value(), ex.what()));
        set_lifecycle_state(ThreadLifecycleState::Terminated);
    } catch (...) {
        PUBSUB_LOG(logger_, FwLogLevel::Error, "{} [{}] terminating due to unknown exception", thread_name_, thread_id_.get_value());
        set_lifecycle_state(ThreadLifecycleState::ShuttingDown);
        reactor_.shutdown(fmt::format("Thread {} [{}] terminated due to unknown exception", thread_name_, thread_id_.get_value()));
        set_lifecycle_state(ThreadLifecycleState::Terminated);
    }
}

void ApplicationThread::run_internal() {
    set_lifecycle_state(ThreadLifecycleState::Started);

    PUBSUB_LOG(logger_, FwLogLevel::Info, "Starting thread {}", thread_name_);

    BackoffWithYield backoff;

    for (;;) {
        // Optional pause mechanism
        if (is_paused_.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
            continue;
        }

        // Physical/lifecycle running state: hard stop once we leave the running band
        if (!is_running()) {
            break;
        }

        // If the Reactor has begun shutdown, this thread should exit promptly
        if (!reactor_.is_running()) {
            PUBSUB_LOG(logger_, FwLogLevel::Warning, "Thread {} detected Reactor shutdown, exiting", thread_name_);
            break;
        }

        // Defensive: queue must exist while the thread is running
        if (message_queue_ == nullptr) {
            PUBSUB_LOG(logger_, FwLogLevel::Error, "Thread {} no longer has message queue, shutting down.", thread_name_);
            break;
        }

        // Main dequeue path
        auto maybe_msg = message_queue_->dequeue();
        if (!maybe_msg.has_value()) {
            // No work available: apply backoff
            backoff.pause();
            continue;
        }

        // We successfully dequeued a message: reset backoff
        backoff.reset();

        EventMessage msg = std::move(*maybe_msg);
        process_message(msg);

        // Application logic may decide this thread is now Terminated
        if (get_lifecycle_state().as_tag() == ThreadLifecycleState::Terminated) {
            break;
        }
    }

    PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {} is shutting down.", thread_name_);
    set_lifecycle_state(ThreadLifecycleState::ShuttingDown);
}

void ApplicationThread::process_message(EventMessage& message) {
    const EventType type = message.type();
    auto tag = static_cast<EventType::EventTypeTag>(type.as_tag());

    auto state = get_lifecycle_state().as_tag();

    const bool is_reactor_event = (tag == EventType::Initial || tag == EventType::AppReady || tag == EventType::Timer || tag == EventType::Termination);
    const bool is_operational = state == ThreadLifecycleState::Operational;

    if (!is_operational && !is_reactor_event) {
        throw PreconditionAssertion("Non-reactor event received before thread is fully operational", __FILE__, __LINE__);
    }

    time_event_started_ = HighResolutionClock::now();

    switch (tag) {
        case EventType::Initial: {
            on_initial_event();
            set_lifecycle_state(ThreadLifecycleState::InitialProcessed);
            PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {}: Initialisation complete", thread_name_);
            break;
        }

        case EventType::AppReady: {
            if (get_lifecycle_state().as_tag() < ThreadLifecycleState::InitialProcessed) {
                throw PreconditionAssertion("Received AppReady event before Initial event was processed", __FILE__, __LINE__);
            }

            on_app_ready_event();

            PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {}: Received AppReady. Moving to operational state.", thread_name_);
            set_lifecycle_state(ThreadLifecycleState::Operational);
            break;
        }

        case EventType::Termination: {
            on_termination_event(message.reason());
            PUBSUB_LOG(logger_, FwLogLevel::Info, "ApplicationThread {} has received Termination event", thread_name_);
            set_lifecycle_state(ThreadLifecycleState::Terminated);
            break;
        }

        case EventType::InterthreadCommunication: {
            on_itc_message(message);
            break;
        }

        case EventType::Timer: {
            on_timer_id_event(message.timer_id());
            break;
        }

        case EventType::PubSubCommunication: {
            on_pubsub_message(message);
            break;
        }

        case EventType::RawSocketCommunication: {
            on_raw_socket_message(message);
            break;
        }

        case EventType::FrameworkPdu: {
            // The inbound slab chunk that carries the raw PDU payload is owned by
            // the reactor's inbound slab allocator. The subclass callback processes
            // the payload and must not hold any references to it after returning.
            // The framework deallocates the chunk here unconditionally so that
            // subclasses never need to do so -- and cannot forget to.
            on_framework_pdu_message(message);
            reactor_.inbound_slab_allocator().deallocate(
                message.slab_id(), const_cast<uint8_t*>(message.payload()));
            break;
        }

        case EventType::ConnectionEstablished: {
            active_connection_ids_.insert(message.connection_id());
            on_connection_established(message.connection_id());
            break;
        }

        case EventType::ConnectionFailed: {
            on_connection_failed(message.reason());
            break;
        }

        case EventType::ConnectionLost: {
            active_connection_ids_.erase(message.connection_id());
            on_connection_lost(message.connection_id(), message.reason());
            break;
        }

        case EventType::None:
        default: {
            PUBSUB_LOG(logger_, FwLogLevel::Warning, "Thread {}: Received unknown or None event type: {}", thread_name_, type.as_string());
            break;
        }
    }
    time_event_finished_ = HighResolutionClock::now();
}

void ApplicationThread::on_timer_id_event(TimerID id) {
    auto it = id_to_name_.find(id);
    if (it == id_to_name_.end()) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning, "Thread {} received timer event for unknown TimerID {}", thread_name_, id.get_value());
        return;
    }

    const std::string& name = it->second;

    // Call the user-overridable handler.
    on_timer_event(name);
}

void ApplicationThread::set_lifecycle_state(ThreadLifecycleState::Tag new_tag) {
    auto old_tag = lifecycle_state_.load(std::memory_order_acquire);

    if (old_tag == new_tag) {
        return;
    }

    PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {} lifecycle transition {} to {}", thread_name_, ThreadLifecycleState::to_string(old_tag),
               ThreadLifecycleState::to_string(new_tag));

    lifecycle_state_.store(new_tag, std::memory_order_release);
}

TimerID ApplicationThread::schedule_timer(const std::string& name, std::chrono::microseconds interval, TimerType type) {
    assert_called_from_owner();

    // If a timer with this name already exists, cancel it first.
    // TODO I am not sure about this. Perhaps it is a precondition violation.
    auto it = name_to_id_.find(name);
    if (it != name_to_id_.end()) {
        cancel_timer(name);
    }

    // Ask Reactor to create and register the timerfd.
    TimerID id = reactor_.allocate_timer_id();
    ReactorControlCommand command(ReactorControlCommand::CommandTag::AddTimer);
    command.timer_name_ = name;
    command.owner_thread_id_ = thread_id_;
    command.timer_id_ = id;
    command.interval_ = interval;
    command.timer_type_ = type;
    reactor_.enqueue_control_command(command);

    // Store mappings for lookup and cancellation.
    name_to_id_[name] = id;
    id_to_name_[id] = name;

    return id;
}

void ApplicationThread::enqueue_send_pdu_command(ConnectionID conn_id, int slab_id, void* chunk, uint32_t payload_bytes) {
    ReactorControlCommand cmd(ReactorControlCommand::CommandTag::SendPdu);
    cmd.connection_id_  = conn_id;
    cmd.allocator_      = &outbound_allocator_;
    cmd.slab_id_        = slab_id;
    cmd.pdu_chunk_ptr_  = chunk;
    cmd.pdu_byte_count_ = payload_bytes;
    reactor_.enqueue_control_command(cmd);
}

} // namespace pubsub_itc_fw

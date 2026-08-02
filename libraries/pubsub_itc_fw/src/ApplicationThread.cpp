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
#include <vector>

#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <fmt/format.h>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BackoffWithYield.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/HighResolutionClock.hpp>
#include <pubsub_itc_fw/LockFreeMessageQueue.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/PduParser.hpp>
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
                   "or the thread refused to stop. Terminating.",
                   thread_name_);
        std::terminate();
    }

    // Note: We do not tell the reactor to deregister the thread.
    // The reactor owns the threads.
    if (notify_fd_ != -1) {
        ::close(notify_fd_);
        notify_fd_ = -1;
    }
}

ApplicationThread::ApplicationThread(ConstructorToken, QuillLogger& logger, Reactor& reactor, std::string thread_name, ThreadID thread_id,
                                     const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config,
                                     const ApplicationThreadConfiguration& thread_config)
    : logger_(logger)
    , reactor_(reactor)
    , outbound_allocator_(thread_config.outbound_slab_size)
    , decode_arena_buffer_()
    , time_event_started_()
    , time_event_finished_()
    , thread_name_(std::move(thread_name))
    , thread_id_(thread_id)
    , thread_(nullptr) {
    if (thread_id.get_value() == 0) {
        throw PreconditionAssertion("ThreadID of zero is reserved for the reactor", __FILE__, __LINE__);
    }

    // Registered here rather than in the initialiser list because the handle is a value: it
    // default-constructs unbound, so there is nothing to initialise until the scope is known
    // to be non-empty. An unnamed thread records nothing, which is what keeps two threads in
    // one process from composing the same key. The application and component tokens are not
    // supplied here at all -- the endpoint takes them from configuration, so framework code
    // cannot name the wrong component.
    if (!thread_config.metrics_scope.empty()) {
        framework_pdu_counter_ = reactor_.metrics().register_counter(thread_config.metrics_scope.c_str(), "framework_pdu_messages_total",
                                                                     "Framework PDU messages delivered to an application thread");
    }

    // resize(), not reserve(): the buffer is a fixed scratch arena addressed via
    // data()+size(), so size() must report the usable length. With reserve() the
    // capacity is set but size() stays 0, and a BumpAllocator built from
    // data()+size() lands in measuring mode -- it can allocate nothing, so decoding
    // any message that populates a list<>/optional field fails. (The integration
    // tests happened to build their arenas from capacity() and so never caught this.)
    decode_arena_buffer_.resize(thread_config.inbound_decode_arena_size);
    message_queue_ = std::make_unique<LockFreeMessageQueue<EventMessage>>(queue_config, allocator_config);

    notify_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (notify_fd_ == -1) {
        throw PubSubItcException(fmt::format("ApplicationThread {}: eventfd creation failed", thread_name_));
    }

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
        200000; // TSAN is slow
#elif defined(USING_VALGRIND)
        500000; // Valgrind is slower
#else
        20000; // normal builds
#endif

    int iterations = 0;

    while (get_lifecycle_state().as_tag() < ThreadLifecycleState::Started) {
        if (++iterations > max_iterations) {
            throw PubSubItcException(fmt::format("Thread {} failed to reach Started state (startup timeout)", thread_name_));
        }
        backoff.pause();
    }
}

[[nodiscard]] bool ApplicationThread::join_with_timeout(std::chrono::milliseconds timeout) const {
    if (thread_ == nullptr) {
        return false;
    }
    return thread_->join_with_timeout(timeout);
}

pthread_t ApplicationThread::get_pthread_id() const {
    if (thread_ == nullptr) {
        throw PreconditionAssertion("ApplicationThread::get_pthread_id called before start()", __FILE__, __LINE__);
    }
    return thread_->get_pthread_id();
}

void ApplicationThread::register_extra_thread(pthread_t id, std::string name) {
    extra_threads_.push_back({id, std::move(name)});
}

void ApplicationThread::pause() {
    is_paused_.store(true, std::memory_order_relaxed);
}

void ApplicationThread::resume() {
    is_paused_.store(false, std::memory_order_relaxed);
}

void ApplicationThread::enqueue(EventMessage message) {
    message_queue_->enqueue(std::move(message));
    constexpr uint64_t one = 1;
    if (::write(notify_fd_, &one, sizeof(one)) == -1 && errno != EAGAIN) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning, "Thread {}: notify_fd_ write failed errno {}", thread_name_, errno);
    }
}

void ApplicationThread::post_message(ThreadID target_thread_id, EventMessage message) const {
    if (target_thread_id == thread_id_) {
        message_queue_->enqueue(std::move(message));
        constexpr uint64_t one = 1;
        if (::write(notify_fd_, &one, sizeof(one)) == -1 && errno != EAGAIN) {
            PUBSUB_LOG(logger_, FwLogLevel::Warning, "Thread {}: notify_fd_ write failed errno {}", thread_name_, errno);
        }
        return;
    }

    reactor_.route_message(target_thread_id, std::move(message));
}

TimerID ApplicationThread::start_one_off_timer(std::chrono::microseconds interval) {
    return schedule_timer(interval, TimerType(TimerType::SingleShot));
}

TimerID ApplicationThread::start_recurring_timer(std::chrono::microseconds interval) {
    return schedule_timer(interval, TimerType(TimerType::Recurring));
}

void ApplicationThread::cancel_timer(TimerID id) {
    assert_called_from_owner();

    // An unset (default-constructed) id means "no timer" -- e.g. a timer field
    // that was never armed, or one whose one-shot already fired. Cancelling it is
    // a no-op, so avoid sending a command the reactor would only reject.
    if (!id.is_valid()) {
        return;
    }

    PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {} sending cancel timer command to reactor for timer id {}", thread_name_, id.get_value());
    ReactorControlCommand command(ReactorControlCommand::CommandTag::CancelTimer);
    command.owner_thread_id_ = thread_id_;
    command.timer_id_ = id;
    reactor_.enqueue_control_command(command);
}

void ApplicationThread::connect_to_service(const std::string& service_name) const {
    assert_called_from_owner();

    // Resolve the name to a ServiceID here so no std::string rides the control
    // queue. An unknown service is a configuration/programming error -- fail fast
    // rather than defer an asynchronous ConnectionFailed for a name that can never
    // resolve.
    const ServiceID service_id = reactor_.resolve_service(service_name);
    if (!service_id.is_valid()) {
        throw PreconditionAssertion("ApplicationThread::connect_to_service: unknown service '" + service_name + "'", __FILE__, __LINE__);
    }

    ReactorControlCommand command(ReactorControlCommand::CommandTag::Connect);
    command.requesting_thread_id_ = thread_id_;
    command.service_id_ = service_id;
    reactor_.enqueue_control_command(command);
}

void ApplicationThread::commit_raw_bytes(const ConnectionID& conn_id, int64_t bytes_consumed) {
    if (active_connection_ids_.find(conn_id) == active_connection_ids_.end()) {
        throw PreconditionAssertion(fmt::format("ApplicationThread::commit_raw_bytes: ConnectionID {} "
                                                "does not belong to this thread",
                                                conn_id.get_value()),
                                    __FILE__, __LINE__);
    }
    ReactorControlCommand command(ReactorControlCommand::CommandTag::CommitRawBytes);
    command.connection_id_ = conn_id;
    command.bytes_consumed_ = bytes_consumed;
    reactor_.enqueue_control_command(command);
}

void ApplicationThread::request_writable_notification(const ConnectionID& conn_id) {
    if (active_connection_ids_.find(conn_id) == active_connection_ids_.end()) {
        throw PreconditionAssertion(fmt::format("ApplicationThread::request_writable_notification: ConnectionID {} "
                                                "does not belong to this thread",
                                                conn_id.get_value()),
                                    __FILE__, __LINE__);
    }
    ReactorControlCommand command(ReactorControlCommand::CommandTag::RequestWritableNotification);
    command.connection_id_ = conn_id;
    reactor_.enqueue_control_command(command);
}

void ApplicationThread::send_raw(const ConnectionID& conn_id, const void* data, uint32_t size) {
    if (active_connection_ids_.find(conn_id) == active_connection_ids_.end()) {
        throw PreconditionAssertion(fmt::format("ApplicationThread::send_raw: ConnectionID {} "
                                                "does not belong to this thread",
                                                conn_id.get_value()),
                                    __FILE__, __LINE__);
    }

    if (data == nullptr) {
        throw PreconditionAssertion("ApplicationThread::send_raw: data must not be nullptr", __FILE__, __LINE__);
    }
    if (size == 0) {
        throw PreconditionAssertion("ApplicationThread::send_raw: size must be greater than zero", __FILE__, __LINE__);
    }

    auto [slab_id, chunk] = outbound_allocator_.allocate(size);
    std::memcpy(chunk, data, size);

    ReactorControlCommand cmd(ReactorControlCommand::CommandTag::SendRaw);
    cmd.connection_id_ = conn_id;
    cmd.allocator_ = &outbound_allocator_;
    cmd.slab_id_ = slab_id;
    cmd.raw_chunk_ptr_ = chunk;
    cmd.raw_byte_count_ = size;
    reactor_.enqueue_control_command(cmd);
}

// Note: reason is used in the logging macros, but we have to neutralise those macros for clang-tidy
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

    // Wake the thread so it exits epoll_wait promptly instead of waiting for
    // the 1-second safety timeout.
    constexpr uint64_t one = 1;
    if (::write(notify_fd_, &one, sizeof(one)) == -1 && errno != EAGAIN) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning, "Thread {}: notify_fd_ write on shutdown failed errno {}", thread_name_, errno);
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
    const std::string os_name = thread_name_.substr(0, 15);
    pthread_setname_np(pthread_self(), os_name.c_str());

    set_lifecycle_state(ThreadLifecycleState::Started);

    PUBSUB_LOG(logger_, FwLogLevel::Info, "Starting thread {}", thread_name_);

    const int ep = ::epoll_create1(EPOLL_CLOEXEC);
    if (ep == -1) {
        PUBSUB_LOG(logger_, FwLogLevel::Error, "Thread {}: epoll_create1 failed errno {}", thread_name_, errno);
        set_lifecycle_state(ThreadLifecycleState::ShuttingDown);
        return;
    }
    struct epoll_event watch {};
    watch.events = EPOLLIN;
    watch.data.fd = notify_fd_;
    ::epoll_ctl(ep, EPOLL_CTL_ADD, notify_fd_, &watch);

    const bool prioritise = prioritise_data_over_timers();
    std::vector<EventMessage> deferred_timers;

    bool keep_running = true;
    while (keep_running) {
        if (is_paused_.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
            continue;
        }

        if (!is_running()) {
            break;
        }

        if (!reactor_.is_running()) {
            PUBSUB_LOG(logger_, FwLogLevel::Warning, "Thread {} detected Reactor shutdown, exiting", thread_name_);
            break;
        }

        if (message_queue_ == nullptr) {
            PUBSUB_LOG(logger_, FwLogLevel::Error, "Thread {} no longer has message queue, shutting down.", thread_name_);
            break;
        }

        // Drain all available messages before potentially blocking.
        // When prioritise_data_over_timers() is true, Timer events are buffered
        // and processed after all data events in this drain cycle have been handled.
        bool any_processed = false;
        while (keep_running) {
            auto maybe_msg = message_queue_->dequeue();
            if (!maybe_msg.has_value()) {
                break;
            }
            any_processed = true;
            EventMessage msg = std::move(*maybe_msg);
            if (prioritise && msg.type().as_tag() == EventType::Timer) {
                deferred_timers.push_back(std::move(msg));
            } else {
                process_message(msg);
                if (get_lifecycle_state().as_tag() == ThreadLifecycleState::Terminated) {
                    keep_running = false;
                }
            }
        }

        // Process deferred Timer events now that the data queue is exhausted.
        for (auto& timer_msg : deferred_timers) {
            if (!keep_running) {
                break;
            }
            process_message(timer_msg);
            if (get_lifecycle_state().as_tag() == ThreadLifecycleState::Terminated) {
                keep_running = false;
            }
        }
        deferred_timers.clear();

        if (keep_running && !any_processed) {
            // Queue empty: block until a producer signals notify_fd_.  The 1-second
            // timeout is a safety net for shutdown races; normal wakeup is immediate.
            struct epoll_event fired[1];
            const int nfds = ::epoll_wait(ep, fired, 1, 1000);
            if (nfds > 0) {
                uint64_t count = 0;
                if (::read(notify_fd_, &count, sizeof(count)) == -1 && errno != EAGAIN) {
                    PUBSUB_LOG(logger_, FwLogLevel::Warning, "Thread {}: notify_fd_ read failed errno {}", thread_name_, errno);
                }
            }
        }
    }

    ::close(ep);
    PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {} is shutting down.", thread_name_);
    set_lifecycle_state(ThreadLifecycleState::ShuttingDown);
}

void ApplicationThread::process_message(const EventMessage& message) {
    const EventType type = message.type();
    auto tag = static_cast<EventType::EventTypeTag>(type.as_tag());

    auto state = get_lifecycle_state().as_tag();

    const bool is_reactor_event = (tag == EventType::Initial || tag == EventType::AppReady || tag == EventType::Timer || tag == EventType::Termination);
    const bool is_operational = state == ThreadLifecycleState::Operational;
    const bool is_shutting_down = state == ThreadLifecycleState::ShuttingDown;

    if (!is_operational && !is_reactor_event) {
        if (is_shutting_down) {
            // Connection teardown and other application events can legitimately
            // arrive while the thread is winding down. Drop them silently --
            // the thread is about to exit and the callbacks are not meaningful.
            PUBSUB_LOG(logger_, FwLogLevel::Debug, "Thread {}: dropping {} event during shutdown (expected during connection teardown)", thread_name_,
                       type.as_string());
            return;
        }
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
            PUBSUB_LOG(logger_, FwLogLevel::Debug, "Thread {}: Received ITC message", thread_name_);
            on_itc_message(message);
            break;
        }

        case EventType::Timer: {
            PUBSUB_LOG(logger_, FwLogLevel::Debug, "Thread {}: Received timer message", thread_name_);
            on_timer_id_event(message.timer_id());
            break;
        }

        case EventType::RawSocketCommunication: {
            PUBSUB_LOG(logger_, FwLogLevel::Debug, "Thread {}: Received raw socket message", thread_name_);
            on_raw_socket_message(message);
            break;
        }

        case EventType::FrameworkPdu: {
            // The inbound slab chunk that carries the raw PDU payload is owned
            // by the reactor's inbound slab allocator. Ownership is passed to
            // the subclass via on_framework_pdu_message(). The subclass MUST
            // call release_pdu_payload(message) on every code path before the
            // event is dispatched out of scope. This is the manual-release
            // contract.
            //
            // The framework deliberately does NOT auto-release here.
            // Auto-release combined with the subclass's own explicit release would
            // produce a double-free of the slab chunk, which manifests later
            // as a "slab has already been destroyed" exception when the slab's
            // outstanding-allocations counter races into a false zero and the
            // reactor reclaims the slab while live allocations still reference it.
            PUBSUB_LOG(logger_, FwLogLevel::Debug, "Thread {}: Received PDU message", thread_name_);
            framework_pdu_counter_.increment();
            on_framework_pdu_message(message);
            break;
        }

        case EventType::ConnectionEstablished: {
            PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {}: Received connection established message", thread_name_);
            active_connection_ids_.insert(message.connection_id());
            on_connection_established(message.connection_id());
            break;
        }

        case EventType::ConnectionFailed: {
            PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {}: Received connected failed message", thread_name_);
            on_connection_failed(message.reason());
            break;
        }

        case EventType::ConnectionLost: {
            PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {}: Received connection lost message", thread_name_);
            active_connection_ids_.erase(message.connection_id());
            on_connection_lost(message.connection_id(), message.reason());
            break;
        }

        case EventType::ConnectionWritable: {
            on_connection_writable(message.connection_id());
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
    // The framework does not track timer identities beyond the id; the id is
    // handed straight to the user-overridable handler, which recognises it by
    // comparing against the ids it retained when scheduling.
    on_timer_event(id);
}

void ApplicationThread::on_connection_established(ConnectionID) {}

void ApplicationThread::set_lifecycle_state(ThreadLifecycleState::Tag new_tag) {
    auto old_tag = lifecycle_state_.load(std::memory_order_acquire);

    if (old_tag == new_tag) {
        return;
    }

    PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {} lifecycle transition {} to {}", thread_name_, ThreadLifecycleState::to_string(old_tag),
               ThreadLifecycleState::to_string(new_tag));

    lifecycle_state_.store(new_tag, std::memory_order_release);
}

TimerID ApplicationThread::schedule_timer(std::chrono::microseconds interval, TimerType type) {
    assert_called_from_owner();

    // Ask Reactor to create and register the timerfd. The reactor allocates a
    // globally unique id, so there is no name-based deduplication here: callers
    // that want to replace a timer cancel it (by id) first.
    TimerID id = reactor_.allocate_timer_id();
    ReactorControlCommand command(ReactorControlCommand::CommandTag::AddTimer);
    command.owner_thread_id_ = thread_id_;
    command.timer_id_ = id;
    command.interval_ = interval;
    command.timer_type_ = type;
    reactor_.enqueue_control_command(command);

    return id;
}

void ApplicationThread::enqueue_send_pdu_command(const ConnectionID& conn_id, int slab_id, void* chunk, uint32_t payload_bytes) {
    ReactorControlCommand cmd(ReactorControlCommand::CommandTag::SendPdu);
    cmd.connection_id_ = conn_id;
    cmd.allocator_ = &outbound_allocator_;
    cmd.slab_id_ = slab_id;
    cmd.pdu_chunk_ptr_ = chunk;
    cmd.pdu_byte_count_ = payload_bytes;
    reactor_.enqueue_control_command(cmd);
}

void ApplicationThread::install_inline_pdu_handler(ConnectionID conn_id, std::function<void(PduParser*, PduFramer*)> installer) {
    ReactorControlCommand cmd(ReactorControlCommand::CommandTag::InstallInlinePduHandler);
    cmd.connection_id_ = std::move(conn_id);
    cmd.inline_handler_installer_ = std::move(installer);
    reactor_.enqueue_control_command(std::move(cmd));
}

void ApplicationThread::release_pdu_payload(const EventMessage& message) const {
    reactor_.inbound_slab_allocator().deallocate(message.slab_id(), const_cast<uint8_t*>(message.payload()));
}

} // namespaces

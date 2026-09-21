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
    spin_before_block_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(thread_config.spin_before_block).count();
    if (spin_before_block_ns_ < 0) {
        throw PreconditionAssertion(fmt::format("ApplicationThread {}: spin_before_block must not be negative", thread_name_), __FILE__, __LINE__);
    }
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

        // Bounds are set here rather than in each component's TOML because this measures the
        // framework's own hand-off, which costs the same order of magnitude whatever the
        // application above it does. They span from a hundred nanoseconds -- below any
        // plausible queue push -- to a hundred milliseconds, so a thread that is merely busy
        // and a thread that has stopped being scheduled both land somewhere readable rather
        // than both landing in the overflow bucket.
        const std::vector<double> itc_queue_latency_buckets = {
            100.0,    250.0,    500.0,    1000.0,    2500.0,    5000.0,    10000.0,    25000.0,    50000.0,
            100000.0, 250000.0, 500000.0, 1000000.0, 2500000.0, 5000000.0, 10000000.0, 50000000.0, 100000000.0,
        };
        itc_queue_latency_histogram_ =
            reactor_.metrics().register_histogram(thread_config.metrics_scope.c_str(), "itc_queue_latency_nanoseconds",
                                                  "Nanoseconds a message spent between being enqueued and being dispatched", itc_queue_latency_buckets);
        itc_queue_latency_unstamped_counter_ =
            reactor_.metrics().register_counter(thread_config.metrics_scope.c_str(), "itc_queue_latency_unstamped_total",
                                                "Messages dispatched with no enqueue stamp, so excluded from itc_queue_latency_nanoseconds");

        // How deep the queue was, recorded for the same messages the latency histogram
        // records, so that the pair answers a question neither answers alone: a message that
        // waited a long time behind nothing was waiting for this thread to be scheduled,
        // and one that waited a long time behind a hundred others was waiting its turn.
        // Those two call for opposite remedies and are indistinguishable from the latency
        // alone.
        //
        // Counts rather than durations, so the bounds are small integers and are set here
        // for the same reason the latency bounds are: this is the framework's own hand-off,
        // and its shape does not depend on the application above it. The top bound is above
        // the default high watermark of 64, so a queue past the point where the watermark
        // handler fires still lands somewhere readable rather than in the overflow bucket.
        const std::vector<double> itc_queue_depth_buckets = {
            0.0, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0, 256.0, 1024.0,
        };
        itc_queue_depth_histogram_ =
            reactor_.metrics().register_histogram(thread_config.metrics_scope.c_str(), "itc_queue_depth",
                                                  "Messages still waiting on the queue when one was taken off for dispatch", itc_queue_depth_buckets);
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
    // Stamped as late as possible, so what the histogram measures is the hand-off rather
    // than the caller's work building the message. Reactor::route_message funnels every
    // cross-thread post through here, which is what makes one stamp site sufficient.
    message.set_enqueued_ns(HighResolutionClock::now().time_since_epoch().count());
    message_queue_->enqueue(std::move(message));
    constexpr uint64_t one = 1;
    if (::write(notify_fd_, &one, sizeof(one)) == -1 && errno != EAGAIN) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning, "Thread {}: notify_fd_ write failed errno {}", thread_name_, errno);
    }
}

void ApplicationThread::post_message(ThreadID target_thread_id, EventMessage message) const {
    if (target_thread_id == thread_id_) {
        // The one path that does not go through enqueue() above, so it stamps for itself.
        // Missing this is how two thirds of a distribution ends up in the overflow bucket.
        message.set_enqueued_ns(HighResolutionClock::now().time_since_epoch().count());
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
    // Last thing, on every path including both catches, which do not rethrow. Anybody asking
    // whether this thread is still executing is asking about exactly this.
    thread_exited_.store(true, std::memory_order_release);
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

            // Read here rather than where the latency is recorded, because here is the moment
            // the number describes: this message has just been taken off, so the count is how
            // many are still waiting behind it. A Timer event that prioritise_data_over_timers()
            // defers is dispatched later in this same drain, by which time the queue has moved
            // on and a reading taken then would describe a different moment.
            //
            // Guarded on the enqueue stamp so this histogram and itc_queue_latency_nanoseconds
            // observe exactly the same messages. Two histograms over two different populations
            // cannot be read against each other, which is the only reason to have this one.
            if (msg.enqueued_ns() != 0) {
                itc_queue_depth_histogram_.observe(static_cast<double>(message_queue_->size()));
            }
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

        // Poll the queue briefly before blocking, when configured to. Work arriving inside
        // the window is taken with no system call, no scheduler involvement and no core to
        // wake; work arriving after it costs exactly what it cost before. Nothing is consumed
        // here -- finding the queue non-empty just restarts the outer loop so the ordinary
        // drain path handles the message, including the timer prioritisation above.
        //
        // The producer has already signalled notify_fd_, so the counter is non-zero and a
        // later epoll_wait returns at once. That costs one spurious wakeup and no messages.
#ifndef USING_VALGRIND
        if (keep_running && !any_processed && spin_before_block_ns_ > 0 && message_queue_ != nullptr) {
            spins_entered_.fetch_add(1, std::memory_order_relaxed);
            const int64_t spin_deadline_ns = HighResolutionClock::now().time_since_epoch().count() + spin_before_block_ns_;
            // cpu_relax rather than BackoffWithYield, which was tried first and measured no
            // better than blocking. That class yields once its first tier is exhausted, and
            // on a fast core that happens well inside this window; a yield is itself a
            // voluntary context switch, so it simply traded one context switch per message
            // for several. BackoffWithYield remains right for an unbounded wait, where
            // standing aside eventually matters more. This wait is bounded by the deadline.
            while (message_queue_->empty()) {
                if (HighResolutionClock::now().time_since_epoch().count() >= spin_deadline_ns) {
                    break;
                }
                cpu_relax();
            }
            if (!message_queue_->empty()) {
                spins_caught_.fetch_add(1, std::memory_order_relaxed);
                // Consume the producer's notification. It was written when the message was
                // enqueued, and the spin found the message without going near epoll_wait, so
                // without this the counter stays raised and the next wait returns at once on
                // a signal for work already done -- a wasted system call per message, which
                // is most of what the spin was meant to save. Anything enqueued after this
                // read raises the counter again, so no wakeup is lost.
                uint64_t drained = 0;
                if (::read(notify_fd_, &drained, sizeof(drained)) == -1 && errno != EAGAIN) {
                    PUBSUB_LOG(logger_, FwLogLevel::Warning, "Thread {}: notify_fd_ read failed after spin, errno {}", thread_name_, errno);
                }
                continue;
            }
        }
#endif

        if (keep_running && !any_processed) {
            waits_blocked_.fetch_add(1, std::memory_order_relaxed);
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
    // First act, before any dispatch work, so the reading is the queue wait and the wakeup
    // and nothing of ours. A message with no stamp is counted, not recorded: treating a zero
    // stamp as a real one would record the time since the monotonic epoch and put the whole
    // family into the overflow bucket.
    if (message.enqueued_ns() != 0) {
        const int64_t queued_ns = HighResolutionClock::now().time_since_epoch().count() - message.enqueued_ns();
        if (queued_ns >= 0) {
            itc_queue_latency_histogram_.observe(static_cast<double>(queued_ns));
        }
    } else {
        itc_queue_latency_unstamped_counter_.increment();
    }

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

void ApplicationThread::observe_wire_crossing(int64_t sent_at_ns) {
    reactor_.observe_wire_crossing(sent_at_ns);
}

void ApplicationThread::enqueue_send_pdu_command(const ConnectionID& conn_id, SlabHandle slab_id, void* chunk, uint32_t payload_bytes,
                                                 MemberIsWaitingFlag member_is_waiting) {
    ReactorControlCommand cmd(ReactorControlCommand::CommandTag::SendPdu);
    cmd.on_order_path_ = member_is_waiting.is_set();
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

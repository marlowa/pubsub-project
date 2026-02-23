#include <pubsub_itc_fw/Reactor.hpp>

#include <pubsub_itc_fw/Backoff.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/MillisecondClock.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace pubsub_itc_fw {

Reactor::Reactor(const ReactorConfiguration& config, QuillLogger& logger)
    : handlers_{}, threads_{}, threads_by_thread_id_{}, config_(config), logger_(logger) {}

void Reactor::route_message(ThreadID target_id, EventMessage message) {
    // Reject attempt to route event message before reactor initialization is complete
    if (!initialization_complete_.load(std::memory_order_acquire)) {
        throw PreconditionAssertion(fmt::format("message posted before Reactor initialization completed, event type {}", message.type().as_string()), __FILE__,
                                    __LINE__);
    }

    // Lookup target thread
    auto it = threads_by_thread_id_.find(target_id);

    if (it == threads_by_thread_id_.end()) {
        // Unknown target: drop safely
        return;
    }

    auto* target = it->second.get();
    // If target is finished, drop safely
    if (!target->is_running()) {
        return;
    }

    auto tag = message.type().as_tag();
    const bool is_reactor_event = tag == EventType::Initial || tag == EventType::AppReady;
    const auto lifecycle_state = target->get_lifecycle_state().as_tag();
    if (!is_reactor_event && lifecycle_state != ThreadLifecycleState::Operational) {
        if (lifecycle_state == ThreadLifecycleState::ShuttingDown || lifecycle_state == ThreadLifecycleState::Terminated) {
            return;
        }
        throw PubSubItcException(fmt::format("Attempted to route non-reactor event {} to non-operational thread", message.type().as_string()));
    }

    // Lookup origin thread
    ThreadID origin_id = message.originating_thread_id();
    auto origin_it = threads_by_thread_id_.find(origin_id);

    // If origin is unknown or finished, drop safely
    if (origin_it == threads_by_thread_id_.end() || !origin_it->second->is_running()) {
        return;
    }

    // Enqueue into target queue
    target->get_queue().enqueue(std::move(message));
}

int Reactor::run() {
    is_finished_.store(false, std::memory_order_release);
    shutdown_reason_ = "";

    initialize_threads();

    event_loop();

    finalize_threads_after_shutdown();

    // TODO we should exit with a non-zero status if any thread got a critical error.
    return 0;
}

void Reactor::shutdown(const std::string& reason) {
    // 1. Mark Reactor as shutting down
    is_finished_.store(true, std::memory_order_release);
    shutdown_reason_ = reason;

    // 2. Broadcast Shutdown event to all threads
    for (auto& [name, thread] : threads_) {
        EventMessage shutdown_msg = EventMessage::create_reactor_event(EventType(EventType::Termination));
        thread->post_message(thread->get_thread_id(), std::move(shutdown_msg));
    }
}

void Reactor::finalize_threads_after_shutdown() {
    // 1. Wait for each thread to exit its run loop (bounded)
    for (auto& [name, thread] : threads_) {
        Backoff backoff;
        auto start = MillisecondClock::now();

        while (thread->is_running()) {
            if (MillisecondClock::now() - start > config_.shutdown_timeout_) {
                PUBSUB_LOG(logger_, LogLevel::Error, "Thread {} did not stop within shutdown_timeout", name);
                break;
            }
            backoff.pause();
        }
    }

    // 2. Join each thread (bounded)
    for (auto& [name, thread] : threads_) {
        if (!thread->join_with_timeout(config_.shutdown_timeout_)) {
            PUBSUB_LOG(logger_, LogLevel::Error, "Thread {} failed to join within shutdown_timeout", name);
        }
    }

    // 3. Wait for each thread to reach Terminated (bounded)
    for (auto& [name, thread] : threads_) {
        Backoff backoff;
        auto start = MillisecondClock::now();

        while (thread->get_lifecycle_state().as_tag() != ThreadLifecycleState::Terminated) {
            if (MillisecondClock::now() - start > config_.shutdown_timeout_) {
                PUBSUB_LOG(logger_, LogLevel::Error, "Thread {} did not reach Terminated within shutdown_timeout", name);
                break;
            }
            backoff.pause();
        }
    }
}

std::string Reactor::get_thread_name_from_id(ThreadID id) const {
    auto it = threads_by_thread_id_.find(id);
    if (it == threads_by_thread_id_.end()) {
        throw PreconditionAssertion("Unknown ThreadID in get_thread_name_from_id", __FILE__, __LINE__);
    }
    return it->second->get_thread_name();
}

void Reactor::register_thread(std::shared_ptr<ApplicationThread> thread) {
    // Safety check: ensure we didn't get a null pointer
    if (thread == nullptr) {
        throw PreconditionAssertion("Cannot register a null thread", __FILE__, __LINE__);
    }

    const std::string& name = thread->get_thread_name();
    ThreadID id = thread->get_thread_id();

    if (threads_.find(name) != threads_.end()) {
        throw PreconditionAssertion(fmt::format("Thread name '{}' is already registered", name), __FILE__, __LINE__);
    }

    if (threads_by_thread_id_.find(id) != threads_by_thread_id_.end()) {
        throw PreconditionAssertion(fmt::format("Thread ID '{}' is already registered", id.get_value()), __FILE__, __LINE__);
    }

    threads_[name] = thread;
    threads_by_thread_id_[id] = thread;

    PUBSUB_LOG(logger_, LogLevel::Info, "Registered application thread: {} (ID: {})", name, id.get_value());
}

void Reactor::initialize_threads() {
    // ---------------------------------------------------------------------
    // 1. Start all registered ApplicationThreads
    // ---------------------------------------------------------------------
    for (auto& [name, thread] : threads_) {
        thread->start();
    }

    // ---------------------------------------------------------------------
    // 2. Wait until all threads have entered their run loops
    // ---------------------------------------------------------------------
    for (auto& [name, thread] : threads_) {
        Backoff backoff;
        auto start = MillisecondClock::now();
        while (!thread->is_running()) {
            if (MillisecondClock::now() - start > config_.init_phase_timeout_) {
                shutdown(fmt::format("Thread {} failed to reach Started within init_phase_timeout", thread->get_thread_name()));
                return;
            }
            backoff.pause();
        }
    }

    // ---------------------------------------------------------------------
    // 3. Post Initial event to each thread
    // ---------------------------------------------------------------------
    for (auto& [name, thread] : threads_) {
        EventMessage init_msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
        thread->post_message(thread->get_thread_id(), std::move(init_msg));
    }

    // ---------------------------------------------------------------------
    // 4. Wait until all threads have *successfully* processed Initial
    //    (i.e. state == InitialProcessed). If a thread moves to
    //    ShuttingDown/Terminated instead, treat that as init failure and
    //    trigger shutdown.
    // ---------------------------------------------------------------------
    for (auto& [name, thread] : threads_) {
        Backoff backoff;
        auto start = MillisecondClock::now();

        for (;;) {
            auto state = thread->get_lifecycle_state().as_tag();

            if (state == ThreadLifecycleState::InitialProcessed) {
                // This thread completed its Initial handler successfully.
                break;
            }

            if (state == ThreadLifecycleState::ShuttingDown || state == ThreadLifecycleState::Terminated) {
                // The thread failed during Initial (e.g. threw and its
                // ApplicationThread caught it and called reactor.shutdown()).
                // Treat this as an initialization failure and abort.
                shutdown(fmt::format("Thread {} failed during Initial processing, state = {}",
                                     thread->get_thread_name(), thread->get_lifecycle_state().as_string()));
                return;
            }

            if (MillisecondClock::now() - start > config_.init_phase_timeout_) {
                shutdown(fmt::format("Thread {} failed to reach InitialProcessed within init_phase_timeout",
                                     thread->get_thread_name()));
                return;
            }

            backoff.pause();
        }
    }

    // ---------------------------------------------------------------------
    // 5. Post AppReady event to each thread
    //
    // If initialization has already been aborted (is_finished_ set by a
    // failing thread), do not attempt to send AppReady at all.
    // ---------------------------------------------------------------------
    if (is_finished_.load(std::memory_order_acquire)) {
        return;
    }

    for (auto& [name, thread] : threads_) {
        auto state = thread->get_lifecycle_state().as_tag();
        if (state != ThreadLifecycleState::InitialProcessed) {
            // Defensive: we should never get here, but if we do, abort cleanly
            // instead of throwing from AppReady fan-out.
            shutdown(fmt::format("Thread {} not InitialProcessed at AppReady fan-out, state = {}",
                                 thread->get_thread_name(), thread->get_lifecycle_state().as_string()));
            return;
        }

        EventMessage ready_msg = EventMessage::create_reactor_event(EventType(EventType::AppReady));
        thread->post_message(thread->get_thread_id(), std::move(ready_msg));
    }

    // 5.5 Wait until all threads are Operational
    for (auto& [name, thread] : threads_) {
        Backoff backoff;
        auto start = MillisecondClock::now();
        while (thread->get_lifecycle_state().as_tag() < ThreadLifecycleState::Operational) {
            if (MillisecondClock::now() - start > config_.init_phase_timeout_) {
                shutdown(fmt::format("Thread {} failed to reach Operational within init_phase_timeout",
                                     thread->get_thread_name()));
                return;
            }
            backoff.pause();
        }
    }

    // 5.6 Mark initialization complete
    initialization_complete_.store(true, std::memory_order_release);
}

void Reactor::event_loop() {
    // ---------------------------------------------------------------------
    // Main loop (placeholder until epoll/timers/sockets are added)
    // ---------------------------------------------------------------------
    {
        Backoff backoff;
        while (!is_finished_.load(std::memory_order_acquire)) {
            backoff.pause();
        }
    }
}

void Reactor::check_for_inactive_threads() {}

void Reactor::check_for_inactive_sockets() {}

} // namespace pubsub_itc_fw

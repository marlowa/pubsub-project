// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.

#include <array>
#include <atomic>
#include <memory>
#include <functional>
#include <utility>
#include <vector>
#include <mutex>

#include <cerrno>
#include <cstdint>

#include <sys/types.h> // for ssize_t
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <fmt/format.h>

#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/Backoff.hpp>
#include <pubsub_itc_fw/EventHandler.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/HighResolutionClock.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/MillisecondClock.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorLifecycleState.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/Timer.hpp>
#include <pubsub_itc_fw/TimerHandler.hpp>
#include <pubsub_itc_fw/TimerType.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

Reactor::~Reactor() {
    if (epoll_fd_ != -1) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (wake_fd_ != -1) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
}

Reactor::Reactor(const ReactorConfiguration& reactor_configuration, QuillLogger& logger)
    : handlers_{}
    , threads_{}
    , threads_by_thread_id_{}
    , command_queue_(reactor_configuration.command_queue_config_, reactor_configuration.command_allocator_config_)
    , config_(reactor_configuration)
    , logger_(logger) {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == -1) {
        throw PubSubItcException("epoll_create1 failed in Reactor constructor");
    }

    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ == -1) {
        throw PubSubItcException("eventfd for wake_fd failed in Reactor constructor");
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wake_fd_;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) == -1) {
        throw PubSubItcException("epoll_ctl ADD failed for wake_fd in Reactor constructor");
    }

    auto timer_id = allocate_timer_id();
    create_timer_fd(timer_id, "Backstop", ThreadID(system_thread_id_value), config_.inactivity_check_interval_, TimerType::Recurring);
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "backstop timer created");
}

void Reactor::route_message(ThreadID target_id, EventMessage message) {
    // 1. Identify if this is a system bootstrap message
    const auto tag = message.type().as_tag();
    const bool is_reactor_event = (tag == EventType::Initial || tag == EventType::AppReady || tag == EventType::Timer || tag == EventType::Termination);

    // 2. Initialization gate
    if (!initialization_complete_.load(std::memory_order_acquire) && !is_reactor_event) {
        throw PreconditionAssertion(fmt::format("Standard message posted before Reactor initialization completed, event type {}", message.type().as_string()),
                                    __FILE__, __LINE__);
    }

    // 3. Fast-path lookup (non-owning pointer)
    ApplicationThread* target = nullptr;
    {
        auto it = fast_path_threads_.find(target_id);
        if (it == fast_path_threads_.end()) {
            return; // No such thread
        }
        target = it->second;
    }

    if (target == nullptr) {
        return;
    }

    // 4. Physical state check
    if (!target->is_running()) {
        return;
    }

    // 5. Lifecycle state guard
    const auto lifecycle_state = target->get_lifecycle_state().as_tag();
    if (!is_reactor_event) {
        // Drop silently if shutting down.
        if (lifecycle_state == ThreadLifecycleState::ShuttingDown || lifecycle_state == ThreadLifecycleState::Terminated) {
            return;
        }

        if (lifecycle_state != ThreadLifecycleState::Operational) {
            throw PubSubItcException(fmt::format("Attempted to route non-reactor event {} to non-operational thread {} state {}", message.type().as_string(),
                                                 target->get_thread_name(), target->get_lifecycle_state().as_string()));
        }
    }

    // 6. Origin validation (unchanged)
    if (message.originating_thread_id().get_value() != system_thread_id_value) {
        ApplicationThread* origin = nullptr;
        {
            const std::lock_guard<std::mutex> lock(thread_registry_mutex_);
            auto origin_it = fast_path_threads_.find(message.originating_thread_id());
            if (origin_it != fast_path_threads_.end()) {
                origin = origin_it->second;
            }
        }

        if (origin == nullptr || !origin->is_running()) {
            return;
        }
    }

    // 7. Enqueue into target queue (hot path)
    target->get_queue().enqueue(std::move(message));
}

int Reactor::run() {
    lifecycle_.store(ReactorLifecycleState::Running, std::memory_order_release);
    shutdown_reason_ = "";

    if (!initialize_threads()) {
        lifecycle_.store(ReactorLifecycleState::FinalizingThreads, std::memory_order_release);
        finalize_threads_after_shutdown();
        lifecycle_.store(ReactorLifecycleState::Finished, std::memory_order_release);
        return -1;
    }

    event_loop();
    cancel_all_timer_fds_for_thread(ThreadID(system_thread_id_value));
    lifecycle_.store(ReactorLifecycleState::FinalizingThreads, std::memory_order_release);
    finalize_threads_after_shutdown();
    lifecycle_.store(ReactorLifecycleState::Finished, std::memory_order_release);

    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "Reactor::run has finished. Returning.");

    // TODO we should exit with a non-zero status if any thread got a critical error.
    return 0;
}

void Reactor::shutdown(const std::string& reason) {
    // Note: if the reactor is already shutting down we cannot safely use the quill logger.

    auto prev = lifecycle_.exchange(ReactorLifecycleState::ShutdownRequested);
    if (prev == ReactorLifecycleState::ShutdownRequested ||
        prev == ReactorLifecycleState::FinalizingThreads ||
        prev == ReactorLifecycleState::Finished) {
        return; // already shutting down or done
    }

    PUBSUB_LOG(logger_, FwLogLevel::Info, "Reactor::shutdown entered. Reason: {}", reason);

    // 1. Mark Reactor as shutting down
    lifecycle_.store(ReactorLifecycleState::ShutdownRequested, std::memory_order_release);
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "Reactor::shutdown is requested, event_loop will finish");
    shutdown_reason_ = reason;

    // wake epoll_wait immediately
    if (wake_fd_ == -1) {
        PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "Reactor::shutdown wake_fd == -1");
    } else {
        uint64_t one = 1;
        PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "in Reactor::shutdown, writing wake_fd");
        const ssize_t n = write(wake_fd_, &one, sizeof(one));
        if (n == sizeof(one)) {
            PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "in Reactor::shutdown, wrote wake_fd");
        } else {
            [[maybe_unused]] auto err = errno;
            PUBSUB_LOG(logger_, FwLogLevel::Error, "in Reactor::shutdown, failed to write wake_fd, errno {} [{}]",
                       err, StringUtils::get_error_string(err));
        }
    }
}

void Reactor::finalize_threads_after_shutdown() {
    // 1. Snapshot under lock
    PUBSUB_LOG(logger_, FwLogLevel::Info, "finalize_threads_after_shutdown entered: threads_.size()={}, shutdown_timeout_={}ms",
               threads_.size(), config_.shutdown_timeout_.count());

    std::vector<std::shared_ptr<ApplicationThread>> snapshot;
    {
        const std::lock_guard<std::mutex> lock(thread_registry_mutex_);
        snapshot.reserve(threads_.size());
        for (const auto& [name, thread] : threads_) {
            if (thread) {
                snapshot.push_back(thread);
            }
        }
    }

    // 1b. Cancel all timers for all threads in this reactor
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "finalize_threads_after_shutdown canceling timers");
    for (auto& thread : snapshot) {
        cancel_all_timer_fds_for_thread(thread->get_thread_id());
    }

    // 2. Wait for run loops to exit
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "finalize_threads_after_shutdown step 2");
    for (auto& thread : snapshot) {
        Backoff backoff;
        auto start = MillisecondClock::now();
        while (thread->is_running()) {
            if (MillisecondClock::now() - start > config_.shutdown_timeout_) {
                PUBSUB_LOG(logger_, FwLogLevel::Error, "Thread {} did not stop within shutdown_timeout", thread->get_thread_name());
                break;
            }
            backoff.pause();
        }
    }

    // 3. Join worker threads
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "finalize_threads_after_shutdown step 3");
    for (auto& thread : snapshot) {
        if (!thread->join_with_timeout(config_.shutdown_timeout_)) {
            PUBSUB_LOG(logger_, FwLogLevel::Error,
                "Thread {} failed to join within shutdown_timeout. "
                "The thread is still running and cannot be safely destroyed. "
                "This is a fatal condition in production — the process should be terminated.",
                thread->get_thread_name());
        }
    }

    // 4. Final cleanup under lock
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "finalize_threads_after_shutdown step 4");
    {
        const std::lock_guard<std::mutex> lock(thread_registry_mutex_);
        for (auto it = threads_.begin(); it != threads_.end();) {
            auto& thread = it->second;
            if (!thread) {
                it = threads_.erase(it);
                continue;
            }

            auto state = thread->get_lifecycle_state().as_tag();
            // If the thread has stopped running but is still marked ShuttingDown,
            // promote it to Terminated now that the reactor has fully finalized it.
            if (state == ThreadLifecycleState::ShuttingDown && !thread->is_running()) {
                thread->set_lifecycle_state(ThreadLifecycleState::Terminated);
                state = ThreadLifecycleState::Terminated;
                PUBSUB_LOG(logger_, FwLogLevel::Warning, "Thread {} is shutting down state, will promote to Terminated", thread->get_thread_name());
            }

            if (state == ThreadLifecycleState::Terminated) {
                const ThreadID id = thread->get_thread_id();
                threads_by_thread_id_.erase(id);
                fast_path_threads_.erase(id);
                it = threads_.erase(it);
            } else {
                ++it;
            }
        }
    }
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "finalize_threads_after_shutdown finished");
}

void Reactor::register_thread(std::shared_ptr<ApplicationThread> thread) {
    if (lifecycle_.load(std::memory_order_acquire) != ReactorLifecycleState::NotStarted) {
        throw PreconditionAssertion("register_thread() called after Reactor has started running", __FILE__, __LINE__);
    }

    // Safety check: ensure we didn't get a null pointer
    if (thread == nullptr) {
        throw PreconditionAssertion("Cannot register a null thread", __FILE__, __LINE__);
    }

    const std::string& name = thread->get_thread_name();
    const ThreadID id = thread->get_thread_id();

    const std::lock_guard<std::mutex> lock(thread_registry_mutex_);

    // 1. Check for Name Collision
    auto name_it = threads_.find(name);
    if (name_it != threads_.end()) {
        // With shared_ptr, if it's present, it's active.
        throw PreconditionAssertion(fmt::format("Thread name '{}' is already registered and active.", name), __FILE__, __LINE__);
    }

    // 2. Check for ThreadID Collision
    auto id_it = threads_by_thread_id_.find(id);
    if (id_it != threads_by_thread_id_.end()) {
        throw PreconditionAssertion(fmt::format("Thread ID '{}' is already registered and active.", id.get_value()), __FILE__, __LINE__);
    }

    // 3. Perform the Registration
    threads_[name] = thread;
    threads_by_thread_id_[id] = thread;

    // Fast-path routing pointer (non-owning)
    fast_path_threads_[id] = thread.get();

    PUBSUB_LOG(logger_, FwLogLevel::Info, "Registered application thread: {} (ID: {})", name, id.get_value());
}

// Reactor.cpp
void Reactor::cancel_all_timer_fds_for_thread(ThreadID owner_thread_id) {
    const std::lock_guard<std::mutex> lock(timer_registry_mutex_);

    PUBSUB_LOG(logger_, FwLogLevel::Info, "Reactor::run event loop has finished, will cancel all timers for thread {}", owner_thread_id.get_value());

    auto it_thread = thread_timer_names_.find(owner_thread_id);
    if (it_thread == thread_timer_names_.end()) {
        return;
    }

    auto& timers_by_name = it_thread->second;

    for (auto it = timers_by_name.begin(); it != timers_by_name.end();) {
        const TimerID id = it->second;

        // Look up FD for this timer
        auto id_it = timer_id_to_fd_.find(id);
        if (id_it != timer_id_to_fd_.end()) {
            const int fd = id_it->second;

            // Remove from ID→FD map before touching handlers
            timer_id_to_fd_.erase(id_it);

            // This removes the TimerHandler and closes the fd
            deregister_handler(fd);
        }

        it = timers_by_name.erase(it);
    }

    // No timers left for this thread
    thread_timer_names_.erase(it_thread);
}

bool Reactor::wait_for_all_threads(std::function<bool(const ApplicationThread&)> predicate, const std::string& phase_name) {
    // 1. Snapshot using shared_ptr
    std::vector<std::shared_ptr<ApplicationThread>> thread_snapshots;
    {
        const std::lock_guard<std::mutex> lock(thread_registry_mutex_);
        for (const auto& [name, thread] : threads_) {
            thread_snapshots.push_back(thread);
        }
    }

    for (auto& thread : thread_snapshots) {
        Backoff backoff;
        auto start = MillisecondClock::now();

        while (true) {
            // If the thread is gone, it can't satisfy the predicate, but it's not "active"
            if (!thread) {
                break;
            }

            auto state = thread->get_lifecycle_state().as_tag();

            // Fatal states must be detected immediately
            if (state == ThreadLifecycleState::ShuttingDown || state == ThreadLifecycleState::Terminated) {
                shutdown(fmt::format("Thread {} failed during {}, state = {}", thread->get_thread_name(), phase_name,
                                     ThreadLifecycleState::to_string(state)));
                return false;
            }


            if (predicate(*thread)) {
                break;
            }

            if (MillisecondClock::now() - start > config_.init_phase_timeout_) {
                shutdown(fmt::format("Thread {} timed out during {} within init_phase_timeout", thread->get_thread_name(), phase_name));
                return false;
            }

            backoff.pause();
        }
    }
    return true;
}

/**
 * @brief Helper to send a reactor event to all living threads.
 */
void Reactor::broadcast_reactor_event(EventType::EventTypeTag tag) {
    const std::lock_guard<std::mutex> lock(thread_registry_mutex_);
    for (auto& [name, thread] : threads_) {
        if (thread != nullptr) {
            EventMessage msg = EventMessage::create_reactor_event(EventType(tag));
            // Route via the ID to ensure all route_message safety checks are applied.
            route_message(thread->get_thread_id(), std::move(msg));
        }
    }
}

bool Reactor::initialize_threads() {
    // 1. Surgical Cleanup: Remove all Tombstones
    //    (With shared_ptr, there are no expired entries, but we preserve the structure.)
    {
        const std::lock_guard<std::mutex> lock(thread_registry_mutex_);

        if (threads_.empty()) {
            PUBSUB_LOG_STR(logger_, FwLogLevel::Error, "Reactor has no registered threads to run");
            return false;
        }

        // Cleanup name map
        for (auto it = threads_.begin(); it != threads_.end();) {
            if (it->second == nullptr) {
                it = threads_.erase(it);
            } else {
                ++it;
            }
        }

        // Cleanup ID map
        for (auto it = threads_by_thread_id_.begin(); it != threads_by_thread_id_.end();) {
            if (it->second == nullptr) {
                it = threads_by_thread_id_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 2. Start all threads
    {
        const std::lock_guard<std::mutex> lock(thread_registry_mutex_);
        for (auto& [name, thread] : threads_) {
            if (thread != nullptr) {
                thread->start();
            }
        }
    }

    // 3. Wait for all threads to enter run loops
    if (!wait_for_all_threads([](const ApplicationThread& t) { return t.is_running(); }, "Startup")) {
        return false;
    }

    // 4. Post Initial event and wait for processing completion
    broadcast_reactor_event(EventType::Initial);

    if (!wait_for_all_threads([](const ApplicationThread& t) { return t.get_lifecycle_state().as_tag() == ThreadLifecycleState::InitialProcessed; },
                              "Initial Processing")) {
        return false;
    }

    // 5. Post AppReady event (Check is_finished_ in case a thread aborted during Initial)
    if (lifecycle_.load(std::memory_order_acquire) != ReactorLifecycleState::Running) {
        return false;
    }

    broadcast_reactor_event(EventType::AppReady);

    // 6. Final Wait: Ensure all threads reach the Operational state
    if (!wait_for_all_threads([](const ApplicationThread& t) { return t.get_lifecycle_state().as_tag() >= ThreadLifecycleState::Operational; },
                              "Operational Transition")) {
        return false;
    }

    // 7. Success
    initialization_complete_.store(true, std::memory_order_release);
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "Registered application threads are now Operational. Reactor initialization complete.");

    return true;
}

void Reactor::enqueue_control_command(const ReactorControlCommand& command) {
    command_queue_.enqueue(command);
    uint64_t one = 1;
    ssize_t n{0};

    do {
        n = ::write(wake_fd_, &one, sizeof(one));
    } while (n == -1 && errno == EINTR);

    if (n == -1) {
        if (errno == EAGAIN) {
            // Pipe/eventfd already full — reactor is already awake.
            return;
        }

        // Anything else is a real logic error.
        throw PubSubItcException(fmt::format("Reactor wakeup write() failed: {}", StringUtils::get_errno_string()));
    }
}

// Thread-safe: this function acquires timer_registry_mutex_ internally.
// Callers must NOT hold the mutex. The entire timer registry, including
// next_timer_id_, is protected by this lock.
TimerID Reactor::allocate_timer_id() {
    const std::lock_guard<std::mutex> lock(timer_registry_mutex_);
    TimerID id = next_timer_id_;
    ++next_timer_id_;
    return id;
}

void Reactor::create_timer_fd(TimerID timer_id, const std::string& name, ThreadID owner_thread_id, std::chrono::microseconds interval, TimerType type) {
    const std::lock_guard<std::mutex> lock(timer_registry_mutex_);

    auto& thread_timers = thread_timer_names_[owner_thread_id];
    if (thread_timers.find(name) != thread_timers.end()) {
        throw PreconditionAssertion(fmt::format("Thread {} already has a timer named '{}'", owner_thread_id.get_value(), name), __FILE__, __LINE__);
    }

    // TODO the line below contains a logging error that is not detected during compilation
    PUBSUB_LOG(logger_, FwLogLevel::Info, "Reactor created timer id {}\n", __FILE__, __LINE__, timer_id.get_value());
    const Timer timer(name, owner_thread_id, timer_id, type, interval);
    auto timer_handler = std::make_unique<TimerHandler>(timer, *this);
    thread_timers[name] = timer_id;
    timer_id_to_fd_[timer_id] = timer_handler->get_fd();
    register_handler(std::move(timer_handler));
}

void Reactor::cancel_timer_fd(ThreadID owner_thread_id, TimerID id) {
    const std::lock_guard<std::mutex> lock(timer_registry_mutex_);

    // 1. Find the FD associated with this TimerID
    auto id_it = timer_id_to_fd_.find(id);
    if (id_it == timer_id_to_fd_.end()) {
        // Technically not a precondition failure if it's already gone,
        // but you could throw if you want strictness.
        return;
    }

    const int fd = id_it->second;

    // 2. Access the handler to get the name (the metadata source of truth)
    auto handler_it = handlers_.find(fd);
    if (handler_it == handlers_.end()) {
        // This should never happen if our maps are in sync
        return;
    }

    // We need to cast the base EventHandler to our specific TimerHandler
    auto* timer_handler = static_cast<TimerHandler*>(handler_it->second.get());
    const std::string& timer_name = timer_handler->get_timer().get_name();

    // 3. Verify the precondition: Ensure this thread actually owns this timer name
    auto& thread_timers = thread_timer_names_[owner_thread_id];
    auto name_it = thread_timers.find(timer_name);

    if (name_it == thread_timers.end()) {
        throw PreconditionAssertion(fmt::format("Thread {} does not have a timer named '{}'", owner_thread_id.get_value(), timer_name), __FILE__, __LINE__);
    }

    // 4. Cleanup all registries
    // Remove from the name-tracking map
    thread_timers.erase(name_it);
    if (thread_timers.empty()) {
        thread_timer_names_.erase(owner_thread_id);
    }

    // Remove from the ID-to-FD management map
    timer_id_to_fd_.erase(id_it);

    // 5. De-register from epoll and remove from the handlers_ map
    // This triggers the TimerHandler dtor, which calls ::close(fd)
    deregister_handler(fd);
}

void Reactor::register_handler(std::unique_ptr<EventHandler> handler) {
    if (handler == nullptr) {
        throw PreconditionAssertion("Cannot register null EventHandler", __FILE__, __LINE__);
    }

    int fd = handler->get_fd();

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd; // we key everything by fd

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
        throw PubSubItcException("epoll_ctl ADD failed in register_handler");
    }

    handlers_.emplace(fd, std::move(handler));
}

void Reactor::deregister_handler(int fd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    handlers_.erase(fd);
    ::close(fd);
}

std::string Reactor::get_shutdown_reason() const
{
    // A plain load is fine: shutdown_reason_ is written once during shutdown
    // and then treated as immutable. Returning a copy avoids lifetime issues.
    return shutdown_reason_;
}

void Reactor::event_loop() {
    std::array<epoll_event, 64> events{};

    while (lifecycle_.load(std::memory_order_acquire) == ReactorLifecycleState::Running) {
        PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "event_loop about to call epoll_wait");
        const int nfds = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), -1);
        PUBSUB_LOG(logger_, FwLogLevel::Info, "epoll_wait returned nfds = {}", nfds);
        if (nfds == -1) {
            if (errno == EINTR) {
                continue; // interrupted by signal, retry
            }
            PUBSUB_LOG(logger_, FwLogLevel::Error, "epoll_wait failed in Reactor::event_loop, errno {}", errno);
            break; // will make reactor shutdown
        }

        dispatch_events(nfds, events.data());
    }
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "Event loop has finished");
}

void Reactor::process_control_commands() {
    for (;;) {
        auto maybe_command = command_queue_.dequeue();
        if (!maybe_command.has_value()) {
            break;
        }

        const ReactorControlCommand& command = maybe_command.value();

        PUBSUB_LOG(logger_, FwLogLevel::Info, "Reactor process_control_commands picked up command {}", command.as_string());

        switch (command.as_tag()) {
            case ReactorControlCommand::AddTimer:
                create_timer_fd(command.timer_id_, command.timer_name_, command.owner_thread_id_, command.interval_, command.timer_type_);
                break;

            case ReactorControlCommand::CancelTimer:
                cancel_timer_fd(command.owner_thread_id_, command.timer_id_);
                break;
        }
    }
}

void Reactor::dispatch_events(int nfds, epoll_event* events) {
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "entered dispatch_events");

    for (int i = 0; i < nfds; ++i) {
        const int fd = events[i].data.fd;

        if (fd == wake_fd_) {
            uint64_t dummy{0};
            PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "dispatch_events got event on wake_fd");

            while (::read(wake_fd_, &dummy, sizeof(dummy)) == sizeof(dummy)) {
                PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "dispatch_events draining wake_fd");
                // keep draining until EAGAIN
            }

            process_control_commands();
            continue;
        }

        auto handler_it = handlers_.find(fd);
        if (handler_it == handlers_.end()) {
            PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "dispatch_events unknown fd, ignore defensively");
            continue; // unknown fd, ignore defensively
        }
        PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "dispatch_events about to handle event");
        // TODO There is more to do here
        handler_it->second->handle_event(events[i].events);
        PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "dispatch_events event handled");
    }
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "dispatch_events returning");
}

// Thread-safe under the reactor lifecycle model:
// - No writes occur while the reactor is Running.
// - No reads occur during initialization or shutdown.
ApplicationThread* Reactor::get_fast_path_thread(ThreadID id) const {
    auto it = fast_path_threads_.find(id);
    return (it != fast_path_threads_.end()) ? it->second : nullptr;
}

ThreadLifecycleState::Tag Reactor::get_thread_state(ThreadID id) const {
    ApplicationThread* t = get_fast_path_thread(id);
    if (t == nullptr) {
        return ThreadLifecycleState::Terminated;
    }
    return t->get_lifecycle_state().as_tag();
}

void Reactor::on_housekeeping_tick() {
    auto state = lifecycle_.load(std::memory_order_acquire);
    if (state == ReactorLifecycleState::ShutdownRequested ||
        state == ReactorLifecycleState::FinalizingThreads ||
        state == ReactorLifecycleState::Finished) {
        return;
    }

    check_for_exited_threads();
    check_for_stuck_threads();
    check_for_inactive_sockets();
}

void Reactor::check_for_exited_threads() {
    const std::lock_guard<std::mutex> lock(thread_registry_mutex_);
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "checking for exited threads");
    for (auto& [name, thread] : threads_) {
        if (thread != nullptr) {
            auto state = thread->get_lifecycle_state().as_tag();
            PUBSUB_LOG(logger_, FwLogLevel::Info, "   thread {} state {}", thread->get_thread_name(), thread->get_lifecycle_state().as_string());
            if (state == ThreadLifecycleState::ShuttingDown) {
                auto rstate = lifecycle_.load(std::memory_order_acquire);
                if (rstate == ReactorLifecycleState::Running) {
                    shutdown("Thread " + name + " is shutting down, will terminate reactor");
                    return;
                }
            }
        }
    }
}

void Reactor::check_for_stuck_threads() {
    // Only meaningful while the reactor is running.
    auto rstate = lifecycle_.load(std::memory_order_acquire);
    if (rstate != ReactorLifecycleState::Running) {
        return;
    }

    // TODO we currently have separate inactivity times for ITC and sockets. Maybe only need one.
    // config_.itc_maximum_inactivity_interval_
    // we also have a separate config interval for the init event because that might be costly.

    const std::lock_guard<std::mutex> lock(thread_registry_mutex_);
    PUBSUB_LOG_STR(logger_, FwLogLevel::Info, "checking for stuck threads");
    for (auto& [name, thread] : threads_) {
        if (thread != nullptr) {
            auto state = thread->get_lifecycle_state().as_tag();
            PUBSUB_LOG(logger_, FwLogLevel::Info, "   thread {} state {}", thread->get_thread_name(), thread->get_lifecycle_state().as_string());
            if (state == ThreadLifecycleState::Operational) {
                if (thread->get_time_event_started() <= thread->get_time_event_finished()) {
                    // Callback finished but did it take too long?
                    auto duration = thread->get_time_event_finished() - thread->get_time_event_started();
                    if (duration > config_.itc_maximum_inactivity_interval_) {
                        auto shutdown_reason = fmt::format("Thread {} callback took too long", thread->get_thread_name());
                        shutdown(shutdown_reason);
                        return; // only need to trigger once
                    }
                } else {
                    // Callback did not finish. How long has it been in there?
                    PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {} callback not finished, checking if stuck", thread->get_thread_name());
                    auto now = HighResolutionClock::now();
                    auto duration = now - thread->get_time_event_started();
                    if (duration > config_.itc_maximum_inactivity_interval_) {
                        auto shutdown_reason = fmt::format("Thread {} callback appears to be stuck", thread->get_thread_name());
                        shutdown(shutdown_reason);
                        return; // only need to trigger once
                    }
                }
            } else if (state == ThreadLifecycleState::Started) {
                PUBSUB_LOG(logger_, FwLogLevel::Info, "Thread {} is started but init not complete, checking if stuck", thread->get_thread_name());
                auto now = HighResolutionClock::now();
                auto duration = now - thread->get_time_event_started();
                if (duration > config_.init_phase_timeout_) {
                    auto shutdown_reason = fmt::format("Thread {} callback appears to be stuck during Init", thread->get_thread_name());
                    shutdown(shutdown_reason);
                    return; // only need to trigger once
                }
            }
        }
    }
}

void Reactor::check_for_inactive_sockets() {}
// TODO!
} // namespace pubsub_itc_fw

#include <chrono>
#include <exception>
#include <thread>
#include <utility>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/Backoff.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

ApplicationThread::~ApplicationThread() {
    if (thread_ != nullptr && thread_->joinable()) {
        // Signal the run loop to exit
        set_lifecycle_state(ThreadLifecycleState::ShuttingDown);
        if (message_queue_ != nullptr) {
            message_queue_->shutdown();
        }

        try {
            bool joined = thread_->join_with_timeout(std::chrono::seconds(3));
            if (!joined) {
                auto errorString = fmt::format("Failed to join thread: {} during destruction, will detach", thread_name_);
                PUBSUB_LOG_STR(logger_, LogLevel::Error, errorString);
                // At this point, something is badly wrong; but do NOT release/destroy the queue while the thread may still run.
                thread_->detach();
            }
        } catch (const std::exception& ex) {
            PUBSUB_LOG_STR(logger_, LogLevel::Error, "Failed to join thread: " + thread_name_ + " during destruction: " + ex.what());
        }
    }
}

ApplicationThread::ApplicationThread(QuillLogger& logger,
                                     Reactor& reactor,
                                     const std::string& thread_name,
                                     ThreadID thread_id,
                                     const QueueConfig& queue_config,
                                     const AllocatorConfig& allocator_config)
    : logger_(logger)
    , reactor_(reactor)
    , time_event_started_()
    , time_event_finished_()
    , thread_name_(thread_name)
    , thread_id_(thread_id)
    , thread_(nullptr) {
    message_queue_ = std::make_unique<LockFreeMessageQueue<EventMessage>>(queue_config, allocator_config);
    set_lifecycle_state(ThreadLifecycleState::Created);
}

void ApplicationThread::start() {
    if (thread_ != nullptr) {
        throw PreconditionAssertion("Thread has already been started.", __FILE__, __LINE__);
    }

    thread_ = std::make_unique<ThreadWithJoinTimeout>();
    thread_->start([this]() { run_internal(); });

    // Make sure we do not return until the started thread is in the run loop.
    Backoff backoff;
    while (get_lifecycle_state().as_tag() < ThreadLifecycleState::Started) {
        backoff.pause();
    }
}

[[nodiscard]] bool ApplicationThread::join_with_timeout(
    std::chrono::milliseconds timeout) {
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

TimerID ApplicationThread::start_one_off_timer(
    const std::string& name,
    std::chrono::microseconds interval) {
    (void)name;
    (void)interval;
    // Timer wiring is out of scope here; keep placeholder.
    return TimerID();
}

TimerID ApplicationThread::start_recurring_timer(
    const std::string& name,
    std::chrono::microseconds interval) {
    (void)name;
    (void)interval;
    return TimerID();
}

void ApplicationThread::cancel_timer(const std::string& name) {
    (void)name;
}

void ApplicationThread::shutdown(const std::string& reason) {
    set_lifecycle_state(ThreadLifecycleState::ShuttingDown);
    message_queue_->shutdown();

    PUBSUB_LOG(logger_, LogLevel::Info, "Thread {} received shutdown signal: {}", thread_name_, reason);
    if (thread_ != nullptr && thread_->joinable()) {
        try {
            if (!thread_->join_with_timeout(std::chrono::seconds(2))) {
                PUBSUB_LOG_STR(logger_, LogLevel::Error, "join with timeout failed");
            }
        } catch (const std::exception& ex) {
            PUBSUB_LOG_STR(logger_, LogLevel::Error,
                "Failed to join thread during shutdown: " + thread_name_ + " " + ex.what());
        }
    }
}

void ApplicationThread::run() {
    try {
        run_internal();
    } catch (const std::exception& ex) {
        PUBSUB_LOG(logger_, LogLevel::Error, "{} [{}] terminating due to exception: {}",
                   thread_name_, thread_id_.get_value(), ex.what());
        reactor_.shutdown("thread run function finished abnormally");
    } catch (...) {
        PUBSUB_LOG(logger_, LogLevel::Error, "{} [{}] terminating due to unknown exception",
                   thread_name_, thread_id_.get_value());
        reactor_.shutdown("thread run function finished abnormally");
    }
}

void ApplicationThread::run_internal() {
    set_lifecycle_state(ThreadLifecycleState::Started);

    PUBSUB_LOG(logger_, LogLevel::Info, "Starting thread {}", thread_name_);

    Backoff backoff;

    try {
        for (;;) {
            if (!is_running()) {
                break;
            }

            if (is_paused_.load(std::memory_order_relaxed)) {
                // Because this thread is paused, we want to hint to the scheduler that another thread should run.
                std::this_thread::yield();
                continue;
            }

            if (reactor_.is_finished()) {
                PUBSUB_LOG(logger_, LogLevel::Warning, "Thread {} detected Reactor shutdown, exiting", thread_name_);
                set_lifecycle_state(ThreadLifecycleState::ShuttingDown);
                break;
            }

            if (message_queue_ == nullptr) {
                PUBSUB_LOG(logger_, LogLevel::Error, "Thread {} no longer has message queue, shutting down.", thread_name_);
                set_lifecycle_state(ThreadLifecycleState::ShuttingDown);
                break;
            }

            auto maybe_msg = message_queue_->dequeue();
            if (!maybe_msg.has_value()) {
                backoff.pause();
                continue;
            }

            backoff.reset();
            EventMessage msg = std::move(*maybe_msg);
            process_message(msg);
        }

        PUBSUB_LOG(logger_, LogLevel::Info, "Thread {} is shutting down.", thread_name_);
    } catch (const std::exception& ex) {
        set_lifecycle_state(ThreadLifecycleState::ShuttingDown);
        PUBSUB_LOG(logger_, LogLevel::Error, "{} [{}] terminating due to exception: {}", thread_name_, thread_id_.get_value(), ex.what());
        reactor_.shutdown("thread run function finished abnormally");
    } catch (...) {
        set_lifecycle_state(ThreadLifecycleState::ShuttingDown);
        PUBSUB_LOG(logger_, LogLevel::Error, "{} [{}] terminating due to unknown exception", thread_name_, thread_id_.get_value());
        reactor_.shutdown("thread run function finished abnormally (unknown exception)");
    }
}

void ApplicationThread::process_message(EventMessage& message) {
    const EventType type = message.type();
    auto tag = static_cast<EventType::EventTypeTag>(type.as_tag());

    auto state = get_lifecycle_state().as_tag();

    const bool is_reactor_event = tag == EventType::Initial || tag == EventType::AppReady;
    const bool is_operational = state == ThreadLifecycleState::Operational;

    if (!is_operational && !is_reactor_event) {
        throw PreconditionAssertion(
            "Non-reactor event received before thread is fully operational", __FILE__, __LINE__);
    }

    switch (tag) {
        case EventType::Initial: {
            on_initial_event();
            set_lifecycle_state(ThreadLifecycleState::InitialProcessed);
            PUBSUB_LOG(logger_, LogLevel::Info, "Thread {}: Initialisation complete", thread_name_);
            break;
        }

        case EventType::AppReady: {
            if (get_lifecycle_state().as_tag() < ThreadLifecycleState::InitialProcessed) {
                throw PreconditionAssertion("Received AppReady event before Initial event was processed", __FILE__, __LINE__);
            }

            on_app_ready_event();

            PUBSUB_LOG(logger_, LogLevel::Info, "Thread {}: Received AppReady. Moving to operational state.",
                       thread_name_);
            set_lifecycle_state(ThreadLifecycleState::Operational);
            break;
        }

        case EventType::Termination: {
            on_termination_event(message.reason());
            break;
        }

        case EventType::InterthreadCommunication: {
            on_itc_message(message);
            break;
        }

        case EventType::Timer: {
            on_timer_event(message.timer_id());
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

        case EventType::None:
        default: {
            PUBSUB_LOG(logger_, LogLevel::Warning, "Thread {}: Received unknown or None event type: {}",
                       thread_name_, type.as_string());
            break;
        }
    }
}

void ApplicationThread::set_lifecycle_state(ThreadLifecycleState::Tag new_tag)
{
    auto old_tag = lifecycle_state_.load(std::memory_order_acquire);

    if (old_tag == new_tag) {
        return;
    }

    PUBSUB_LOG(logger_, LogLevel::Info, "Thread {} lifecycle transition {} → {}", thread_name_,
               ThreadLifecycleState::to_string(old_tag), ThreadLifecycleState::to_string(new_tag));

    lifecycle_state_.store(new_tag, std::memory_order_release);
}

} // namespace pubsub_itc_fw

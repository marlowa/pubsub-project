#include <chrono>
#include <exception>
#include <thread>
#include <utility>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

ApplicationThread::~ApplicationThread() {
    if (!run_loop_entered_.load()) {
        // thread has not started yet, so no cleanup to do
        return;
    }

    if (thread_ != nullptr && thread_->joinable()) {
        // Signal the run loop to exit
        is_running_.store(false, std::memory_order_relaxed);
        if (message_queue_ != nullptr) {
            message_queue_->shutdown();
        }

        try {
            bool joined = thread_->join_with_timeout(std::chrono::seconds(3));
            if (!joined) {
                auto errorString = fmt::format("Failed to join thread: {} during destruction", thread_name_);
                PUBSUB_LOG_STR(logger_, LogLevel::Error, errorString);
                // At this point, something is badly wrong; but do NOT
                // release/destroy the queue while the thread may still run.
                // TBD whether to assert/terminate here in production.
                throw PubSubItcException(errorString);
            }
        } catch (const std::exception& ex) {
            PUBSUB_LOG_STR(logger_, LogLevel::Error,
                           "Failed to join thread: " + thread_name_ + " during destruction: " + ex.what());
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
}

void ApplicationThread::start() {
    if (thread_ != nullptr) {
        throw PreconditionAssertion("Thread has already been started.",
                                    __FILE__,
                                    __LINE__);
    }

    thread_ = std::make_unique<ThreadWithJoinTimeout>();
    thread_->start([this]() { run_internal(); });

    // Make sure we do not return until the started thread is in the run loop.
     while (!run_loop_entered_.load(std::memory_order_acquire))
     {
         // TODO might use mm_pause here
         std::this_thread::yield();
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
    is_running_.store(false, std::memory_order_relaxed);
    message_queue_->shutdown();

    // TODO use fmt instead of string addition

    PUBSUB_LOG_STR(logger_, LogLevel::Info,
        "Thread " + thread_name_ + " received shutdown signal: " + reason);

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
    run_loop_entered_.store(true, std::memory_order_release);
    is_running_.store(true, std::memory_order_relaxed);

    PUBSUB_LOG(logger_, LogLevel::Info, "Starting thread {}", thread_name_);

    try {
        for (;;) {
            if (!is_running_.load(std::memory_order_relaxed)) {
                break;
            }

            if (is_paused_.load(std::memory_order_relaxed)) {
                // TODO do a mm_pause on intel machines
                std::this_thread::yield();
                continue;
            }

            if (reactor_.is_finished()) {
                PUBSUB_LOG(logger_, LogLevel::Warning,
                          "Thread {} detected Reactor shutdown, exiting",
                          thread_name_);
                break;
            }

            if (message_queue_ == nullptr) {
                PUBSUB_LOG(logger_, LogLevel::Error, "Thread {} no longer has message queue, shutting down.", thread_name_);
                is_running_.store(false, std::memory_order_relaxed);
                break;
            }

            auto maybe_msg = message_queue_->dequeue();
            if (!maybe_msg.has_value()) {
                // TODO do a mm_pause on intel machines
                std::this_thread::yield();
                continue;
            }

            EventMessage msg = std::move(*maybe_msg);
            process_message(msg);
        }

        PUBSUB_LOG(logger_, LogLevel::Info, "Thread {} is shutting down.", thread_name_);
    } catch (const std::exception& ex) {
        is_running_.store(false, std::memory_order_relaxed);

        PUBSUB_LOG(logger_, LogLevel::Error, "{} [{}] terminating due to exception: {}",
                   thread_name_, thread_id_.get_value(), ex.what());

        reactor_.shutdown("thread run function finished abnormally");
    } catch (...) {
        is_running_.store(false, std::memory_order_relaxed);

        PUBSUB_LOG(logger_, LogLevel::Error,
                   "{} [{}] terminating due to unknown exception",
                   thread_name_, thread_id_.get_value());

        reactor_.shutdown("thread run function finished abnormally (unknown exception)");
    }
}

void ApplicationThread::process_message(EventMessage& message) {
    const EventType type = message.type();

    switch (static_cast<EventType::EventTypeTag>(type.as_tag())) {
        case EventType::Initial: {
            on_initial_event();
            set_has_processed_initial_event();
            PUBSUB_LOG(logger_, LogLevel::Info, "Thread {}: Initialisation complete", thread_name_);
            break;
        }

        case EventType::AppReady: {
            if (!get_has_processed_initial_event()) {
                throw PreconditionAssertion("Received AppReady event before Initial event was processed",
                    __FILE__, __LINE__);
            }

            on_app_ready_event();

            PUBSUB_LOG(logger_, LogLevel::Info, "Thread {}: Received AppReady. Moving to operational state.",
                       thread_name_);
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

} // namespace pubsub_itc_fw

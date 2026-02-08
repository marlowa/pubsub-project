#include <chrono>
#include <exception>
#include <thread>
#include <utility>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

namespace pubsub_itc_fw {

ApplicationThread::~ApplicationThread() {
    if (thread_ != nullptr && thread_->joinable()) {
        try {
            bool joined =
                thread_->join_with_timeout(std::chrono::seconds(3));
            if (!joined) {
                PUBSUB_LOG_STR(logger_,
                               LogLevel::Error,
                               "Failed to join thread: " + thread_name_ +
                                   " during destruction");
            }
        } catch (const std::exception& ex) {
            PUBSUB_LOG_STR(logger_,
                           LogLevel::Error,
                           "Failed to join thread: " + thread_name_ +
                               " during destruction: " + ex.what());
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
    , message_queue_(queue_config, allocator_config)
    , thread_(nullptr) {
}

void ApplicationThread::start() {
    if (thread_ != nullptr) {
        throw PreconditionAssertion("Thread has already been started.",
                                    __FILE__,
                                    __LINE__);
    }

    thread_ = std::make_unique<ThreadWithJoinTimeout>();
    thread_->start([this]() { run_internal(); });
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

void ApplicationThread::post_message(ThreadID target_thread_id,
                                     EventMessage message) {
    (void)target_thread_id; // routing to other threads is Reactor’s job
    message_queue_.enqueue(std::move(message));
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
    message_queue_.shutdown();

    PUBSUB_LOG_STR(
        logger_,
        LogLevel::Info,
        "Thread " + thread_name_ + " received shutdown signal: " + reason);

    if (thread_ != nullptr && thread_->joinable()) {
        try {
            thread_->join();
        } catch (const std::exception& ex) {
            PUBSUB_LOG_STR(
                logger_,
                LogLevel::Error,
                "Failed to join thread during shutdown: " + thread_name_ +
                    " " + ex.what());
        }
    }
}

void ApplicationThread::run_internal() {
    is_running_.store(true, std::memory_order_relaxed);

    PUBSUB_LOG(logger_,
               LogLevel::Info,
               "Starting thread {}",
               thread_name_);

    try {
        for (;;) {
            if (!is_running_.load(std::memory_order_relaxed)) {
                break;
            }

            if (is_paused_.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
                continue;
            }

            auto maybe_msg = message_queue_.dequeue();
            if (!maybe_msg.has_value()) {
                std::this_thread::yield();
                continue;
            }

            EventMessage msg = std::move(*maybe_msg);
            process_message(msg);
        }

        PUBSUB_LOG(logger_,
                   LogLevel::Info,
                   "Thread {} is shutting down.",
                   thread_name_);
    } catch (const std::exception& ex) {
        is_running_.store(false, std::memory_order_relaxed);

        PUBSUB_LOG(logger_,
                   LogLevel::Error,
                   "{} [{}] terminating due to exception: {}",
                   thread_name_,
                   thread_id_.get_value(),
                   ex.what());

        reactor_.shutdown(
            "thread run function finished abnormally");
    } catch (...) {
        is_running_.store(false, std::memory_order_relaxed);

        PUBSUB_LOG(logger_,
                   LogLevel::Error,
                   "{} [{}] terminating due to unknown exception",
                   thread_name_,
                   thread_id_.get_value());

        reactor_.shutdown(
            "thread run function finished abnormally (unknown exception)");
    }
}

void ApplicationThread::on_data_message(const EventMessage& event_message) {
    (void)event_message;
    // Intentionally left as a stub; message handling is in process_message().
}

} // namespace pubsub_itc_fw

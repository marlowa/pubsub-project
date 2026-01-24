#include <stdexcept>
#include <iostream>
#include <utility>
#include <thread>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/Logger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

namespace pubsub_itc_fw {

ApplicationThread::~ApplicationThread() {
    // The destructor ensures the thread is joined.
    if (thread_ && thread_->joinable()) {
        try {
            thread_->joinWithTimeout(std::chrono::seconds(3));
        } catch (const std::system_error& e) {
            // Log the error but do not throw from the destructor.
            PUBSUB_LOG_STR(logger_, LogLevel::Error, "Failed to join thread: " + thread_name_ + " during destruction: " + e.what());
        }
    }
}

ApplicationThread::ApplicationThread(const LoggerInterface& logger,
                                     const std::string& thread_name,
                                     uint32_t low_watermark,
                                     uint32_t high_watermark)
    : logger_(logger),
      thread_name_(thread_name),
      queue_(low_watermark, high_watermark) {
    time_event_started_ = std::chrono::time_point<std::chrono::system_clock>{};
    time_event_finished_ = std::chrono::time_point<std::chrono::system_clock>{};
}

void ApplicationThread::dispatch_message(EventMessage event_message) {
    if (!queue_.try_enqueue(std::move(event_message))) {
// TODO this is wrong
        throw PubSubItcException("Failed to enqueue message. Queue is full.");
    }
}

void ApplicationThread::start(ThreadID thread_id) {
    if (thread_) {
        throw std::runtime_error("Thread has already been started."); // TODO preconditionViolation
    }

    thread_ = std::make_unique<ThreadWithJoinTimeout>([this, thread_id]() {
        thread_id_ = thread_id;
        run();
    });
}

void ApplicationThread::join()
{
    if (thread_ != nullptr && thread_->joinable())
    {
        thread_->join();
    }
}

[[nodiscard]] bool ApplicationThread::join_with_timeout(std::chrono::milliseconds timeout)
{
    if (thread_ == nullptr)
    {
        return false;
    }
    return thread_->join_with_timeout(timeout);
}

bool ApplicationThread::is_running() const noexcept {
    return is_running_.load();
}

bool ApplicationThread::post_message(ThreadID destination_thread_id,
                                     ThreadID originating_thread_id,
                                     void* pointer,
                                     int length,
                                     void* socket_handler) {
#if 0
    // TODO we need to write the code for this
    // This function is a placeholder for a more complex routing mechanism.
    // In a full implementation, this would look up the queue for the
    // destination_thread_id and enqueue the message.
    (void)destination_thread_id;
    (void)originating_thread_id;
    (void)pointer;
    (void)length;
    (void)socket_handler;
#endif
    return false;
}

void ApplicationThread::shutdown(const std::string& reason) {
    is_running_ = false;
    PUBSUB_LOG_STR(logger_, LogLevel::Info, "Thread " + thread_name_ + " received shutdown signal: " + reason);
    if (thread_ && thread_->joinable()) {
        try {
            thread_->join();
        } catch (const std::system_error& e) {
            PUBSUB_LOG_STR(logger_, LogLevel::Error, "Failed to join thread during shutdown: " + thread_name_ + " " + e.what());
        }
    }
}

void ApplicationThread::run()
{
    try
    {
        run_internal();
        is_finished_ = true;
        FWPOC_LOG(logger_, LogLevel::Info, "{} [{}] terminating normally", threadName_, thread_id_.as_string());
    }
    catch (const std::exception& ex)
    {
        FWPOC_LOG(logger_, LogLevel::Error, " {} [{}] terminating due to exception: {}", thread_name_, thread_id_.as_string(), ex.what());
        // TODO we need to cancel any timers created by this thread.
        is_finished_ = true;
        EventMessage terminationEvent("thread run function finished abnormally, finishing all other threads", m_threadID);
        reactor_->shutdown("thread run function finished abnormally");
    }
}

void ApplicationThread::run_internal() {
    PUBSUB_LOG_STR(logger_, LogLevel::Info, "Starting thread " + thread_name_);
    while (is_running_.load()) {
        EventMessage message;
        if (queue_.try_dequeue(message)) {
            process_message(message);
        }
    }
    PUBSUB_LOG_STR(logger_, LogLevel::Info, "Thread " + thread_name_ + " is shutting down.");
}

void ApplicationThread::on_data_message(const EventMessage& event_message) {
    // This method is now a stub, as the main logic has been moved to process_message.
    (void)event_message;
}

} // namespace pubsub_itc_fw

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
          if (!thread_->join_with_timeout(std::chrono::seconds(3))) {
              PUBSUB_LOG_STR(logger_, LogLevel::Error, "Failed to join thread: " + thread_name_ + " during destruction");
          }
        } catch (const std::system_error& e) {
            // Log the error but do not throw from the destructor.
            PUBSUB_LOG_STR(logger_, LogLevel::Error, "Failed to join thread: " + thread_name_ + " during destruction: " + e.what());
        }
    }
}

ApplicationThread::ApplicationThread(const LoggerInterface& logger,
                                     Reactor& reactor,
                                     const std::string& thread_name,
                                     ThreadID thread_id,
                                     uint32_t low_watermark,
                                     uint32_t high_watermark,
                      std::function<void(void* for_client_use)> gone_below_low_watermark_handler,
                      std::function<void(void* for_client_use)> gone_above_high_watermark_handler)
    : logger_(logger),
      reactor_(reactor),
      thread_name_(thread_name),
      thread_id_(thread_id),
      message_queue_(low_watermark, high_watermark,
                     this, gone_below_low_watermark_handler, gone_above_high_watermark_handler) {
    time_event_started_ = std::chrono::time_point<std::chrono::system_clock>{};
    time_event_finished_ = std::chrono::time_point<std::chrono::system_clock>{};
}

void ApplicationThread::start() {
    if (thread_) {
        throw PreconditionAssertion("Thread has already been started.", __FILE__, __LINE__);
    }

    thread_ = std::make_unique<ThreadWithJoinTimeout>([this]() {
        run();
    });
}

#if 0
// APM not sure if join is neededm, would encourage people not to use timeout version
void ApplicationThread::join()
{
    if (thread_ != nullptr && thread_->joinable())
    {
        thread_->join();
    }
}
#endif

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

void ApplicationThread::post_message(ThreadID target_thread_id, EventMessage message)
{
// TODO we need to write code here
}

#if 0
bool ApplicationThread::post_message(ThreadID destination_thread_id,
                                     ThreadID originating_thread_id,
                                     void* pointer,
                                     int length,
                                     void* socket_handler) {
    // TODO we need to write the code for this
    // This function is a placeholder for a more complex routing mechanism.
    // In a full implementation, this would look up the queue for the
    // destination_thread_id and enqueue the message.
    (void)destination_thread_id;
    (void)originating_thread_id;
    (void)pointer;
    (void)length;
    (void)socket_handler;
    return false;
}
#endif

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
        is_running_.store(false);
        FWPOC_LOG(logger_, LogLevel::Info, "{} [{}] terminating normally", thread_name_, thread_id_);
    }
    catch (const std::exception& ex)
    {
        FWPOC_LOG(logger_, LogLevel::Error, " {} [{}] terminating due to exception: {}", thread_name_, thread_id_, ex.what());
        // TODO we need to cancel any timers created by this thread.
        is_running_.store(false);
        EventMessage terminationEvent("thread run function finished abnormally, finishing all other threads", thread_id_);
        reactor_->shutdown("thread run function finished abnormally");
    }
}

void ApplicationThread::run_internal() {
    PUBSUB_LOG(logger_, LogLevel::Info, "Starting thread {}", thread_name_);
    while (is_running_.load()) {
        EventMessage message;
        if (message_queue_.try_dequeue(message)) {
            process_message(message);
        }
    }
    PUBSUB_LOG(logger_, LogLevel::Info, "Thread {} is shutting down.", thread_name_);
}

void ApplicationThread::on_data_message(const EventMessage& event_message) {
    // This method is now a stub, as the main logic has been moved to process_message.
    (void)event_message;
}

} // namespace pubsub_itc_fw

#pragma once

#include <atomic>
#include <string>

#include <pubsub_itc_fw/Reactor.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A lightweight mock of Reactor used for unit testing ApplicationThread.
 *
 * It overrides only the virtual methods that ApplicationThread interacts with:
 *   - shutdown(reason)
 *   - get_thread_name_from_id(id)
 */
class MockReactor : public Reactor
{
public:
    ~MockReactor() override = default;

    MockReactor(const ReactorConfiguration& config, QuillLogger& logger)
        : Reactor(config, logger)
        , shutdown_called_(false)
        , last_shutdown_reason_("")
    {
    }

    void shutdown(const std::string& reason) override
    {
        shutdown_called_.store(true, std::memory_order_release);
        last_shutdown_reason_ = reason;
    }

    std::string get_thread_name_from_id(ThreadID id) const override
    {
        return "MockThread_" + std::to_string(static_cast<int>(id.get_value()));
    }

    bool shutdown_called() const
    {
        return shutdown_called_.load(std::memory_order_acquire);
    }

    const std::string& last_shutdown_reason() const
    {
        return last_shutdown_reason_;
    }

private:
    std::atomic<bool> shutdown_called_;
    std::string last_shutdown_reason_;
};

} // namespace pubsub_itc_fw

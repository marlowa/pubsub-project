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
    {
    }

    int run() override {
        is_finished_.store(false, std::memory_order_release);
        shutdown_reason_ = "";
        return 0;
    }

    std::string get_thread_name_from_id(ThreadID id) const override
    {
        return "MockThread_" + std::to_string(static_cast<int>(id.get_value()));
    }
};

} // namespace pubsub_itc_fw

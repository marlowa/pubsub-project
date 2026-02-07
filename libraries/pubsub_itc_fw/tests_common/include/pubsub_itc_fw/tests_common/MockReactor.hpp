#pragma once

#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>

namespace {

pubsub_itc_fw::ReactorConfiguration reactorConfiguration{};

}

namespace pubsub_itc_fw::tests {

class MockReactor : public Reactor {
public:
  MockReactor(LoggerInterface& logger) : Reactor(reactorConfiguration, logger) {}

    std::atomic<bool> shutdown_called{false};
    std::string shutdown_reason;

    void shutdown(const std::string& reason) override {
        shutdown_called = true;
        shutdown_reason = reason;
    }

    // If ApplicationThread calls this, stub it:
    std::string get_thread_name_from_id(ThreadID id) const override {
        return "mock-thread";
    }
};

} // namespaces


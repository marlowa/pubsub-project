#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.

#include <pubsub_itc_fw/LoggerInterface.hpp>

namespace pubsub_itc_fw::tests {

class MockLogger : public LoggerInterface {
public:
    bool should_log(LogLevel) const override {
        return true;
    }

    void set_log_level(LogLevel) override {
    }

    void flush() const override {
    }

    void set_immediate_flush() override {
    }
};

} // namespace pubsub_itc_fw::tests

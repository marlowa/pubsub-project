#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <memory>

#include <quill/Frontend.h>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/tests_common/TestSink.hpp>

namespace pubsub_itc_fw {

class LoggerWithSink
{
public:
    QuillLogger logger;
    std::shared_ptr<TestSink> sink;

    LoggerWithSink(const std::string& logger_name, const std::string& sink_name) {
        auto sink_base = quill::Frontend::create_or_get_sink<TestSink>(sink_name);
        auto sink_typed = std::static_pointer_cast<TestSink>(sink_base);
        logger = QuillLogger(logger_name, sink_base, FwLogLevel::Debug);
        sink = sink_typed;
    }

private:
    LoggerWithSink() = default;
};

} // namespaces

#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <quill/LogMacros.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

/** @ingroup logging_subsystem */

// In clang-tidy analysis mode the macros are neutralised to avoid false
// positives from the variadic forwarding.
#ifdef CLANG_TIDY
#define PUBSUB_LOG(logger_expr, log_level_expr, fmt, ...) do {} while (0)
#define PUBSUB_LOG_STR(logger_expr, log_level_expr, msg)  do {} while (0)
#else

// PUBSUB_LOG — log a format string with zero or more arguments.
//
// Arguments are serialised into Quill's lock-free ring buffer on the calling
// thread and formatted on the Quill backend thread.  No string construction
// occurs here.
//
// Usage:
//   PUBSUB_LOG(logger, FwLogLevel::Info, "connected to {} port {}", host, port);
#define PUBSUB_LOG(logger_expr, log_level_expr, fmt, ...)                               \
    do {                                                                                \
        auto& _pubsub_logger = (logger_expr);                                           \
        const ::pubsub_itc_fw::FwLogLevel _pubsub_level = (log_level_expr);            \
        if (_pubsub_level == ::pubsub_itc_fw::FwLogLevel::Alert ||                     \
            _pubsub_level == ::pubsub_itc_fw::FwLogLevel::Critical) {                  \
            LOG_CRITICAL(_pubsub_logger.quill_logger(), fmt, ##__VA_ARGS__);            \
        } else if (_pubsub_level == ::pubsub_itc_fw::FwLogLevel::Error) {              \
            LOG_ERROR(_pubsub_logger.quill_logger(), fmt, ##__VA_ARGS__);               \
        } else if (_pubsub_level == ::pubsub_itc_fw::FwLogLevel::Warning) {            \
            LOG_WARNING(_pubsub_logger.quill_logger(), fmt, ##__VA_ARGS__);             \
        } else if (_pubsub_level == ::pubsub_itc_fw::FwLogLevel::Notice ||             \
                   _pubsub_level == ::pubsub_itc_fw::FwLogLevel::Info) {               \
            LOG_INFO(_pubsub_logger.quill_logger(), fmt, ##__VA_ARGS__);                \
        } else if (_pubsub_level == ::pubsub_itc_fw::FwLogLevel::Debug) {              \
            LOG_DEBUG(_pubsub_logger.quill_logger(), fmt, ##__VA_ARGS__);               \
        } else {                                                                        \
            LOG_TRACE_L3(_pubsub_logger.quill_logger(), fmt, ##__VA_ARGS__);            \
        }                                                                               \
    } while (0)

// PUBSUB_LOG_STR — log a single pre-formed string.
//
// Equivalent to PUBSUB_LOG with "{}" as the format string.  Required because
// -Werror=variadic-macros rejects a macro invocation with no variadic args.
//
// Usage:
//   PUBSUB_LOG_STR(logger, FwLogLevel::Warning, some_std_string);
#define PUBSUB_LOG_STR(logger_expr, log_level_expr, msg)                                \
    PUBSUB_LOG(logger_expr, log_level_expr, "{}", msg)

#endif // CLANG_TIDY

#pragma once

#include <quill/LogMacros.h>

#include <pubsub_itc_fw/LoggerUtils.hpp>
#include <pubsub_itc_fw/LogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

/** @ingroup logging_subsystem */

// TODO we need to do something so that criticals, errors etc cause log flushing.
// There is a new way to do this in quill v11.

// Note: In Quill 11.x, the macros are LOG_CRITICAL, LOG_ERROR, etc. (not QUILL_LOG_*)

#ifdef CLANG_TIDY
#define PUBSUB_LOG(logger_expr, log_level_expr, fmt, ...) do {} while (0)
#define PUBSUB_LOG_STR(logger_expr, log_level_expr, msg)  do {} while (0)
#else
#define PUBSUB_LOG(logger_expr, log_level_expr, fmt, ...)                               \
    do {                                                                                \
        auto& logger_local_ref = (logger_expr);                                         \
        ::pubsub_itc_fw::LogLevel level_obj = (log_level_expr);                         \
        if (level_obj == ::pubsub_itc_fw::LogLevel::Alert ||                            \
            level_obj == ::pubsub_itc_fw::LogLevel::Critical) {                         \
            LOG_CRITICAL(logger_local_ref.quill_logger(), fmt, ##__VA_ARGS__);          \
        } else if (level_obj == ::pubsub_itc_fw::LogLevel::Error) {                     \
            LOG_ERROR(logger_local_ref.quill_logger(), fmt, ##__VA_ARGS__);             \
        } else if (level_obj == ::pubsub_itc_fw::LogLevel::Warning) {                   \
            LOG_WARNING(logger_local_ref.quill_logger(), fmt, ##__VA_ARGS__);           \
        } else if (level_obj == ::pubsub_itc_fw::LogLevel::Notice) {                    \
            LOG_INFO(logger_local_ref.quill_logger(), fmt, ##__VA_ARGS__);              \
        } else if (level_obj == ::pubsub_itc_fw::LogLevel::Info) {                      \
            LOG_INFO(logger_local_ref.quill_logger(), fmt, ##__VA_ARGS__);              \
        } else if (level_obj == ::pubsub_itc_fw::LogLevel::Debug) {                     \
            LOG_DEBUG(logger_local_ref.quill_logger(), fmt, ##__VA_ARGS__);             \
        }                                                                               \
    } while (0)

#define PUBSUB_LOG_STR(logger_expr, log_level_expr, msg)                                \
    PUBSUB_LOG(logger_expr, log_level_expr, "{}", msg)
#endif

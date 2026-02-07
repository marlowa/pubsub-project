#pragma once

#include <quill/LogMacros.h>
#include <pubsub_itc_fw/LoggerUtils.hpp>
#include <pubsub_itc_fw/LogLevel.hpp>

#define PUBSUB_LOG(logger_expr, log_level_expr, fmt, ...)                                      \
    do {                                                                                       \
        auto& logger_local_ref = (logger_expr);                                                \
        ::pubsub_itc_fw::LogLevel level_obj{log_level_expr};                                   \
                                                                                               \
        if (logger_local_ref.should_log(level_obj)) {                                          \
            auto* quill_logger = logger_local_ref.quill_logger();                              \
                                                                                               \
            const char* file = ::pubsub_itc_fw::LoggerUtils::leafname(__FILE__);               \
            const char* func = ::pubsub_itc_fw::LoggerUtils::function_name(__PRETTY_FUNCTION__).data(); \
            int line = __LINE__;                                                               \
                                                                                               \
            switch (level_obj.log_level_) {                                                    \
                case ::pubsub_itc_fw::LogLevel::Alert:                                         \
                case ::pubsub_itc_fw::LogLevel::Critical:                                      \
                    LOG_CRITICAL(quill_logger, "{}:{} {} " fmt,                                \
                                 file, line, func, __VA_ARGS__);                               \
                    quill_logger->flush_log();                                                 \
                    break;                                                                     \
                                                                                               \
                case ::pubsub_itc_fw::LogLevel::Error:                                         \
                    LOG_ERROR(quill_logger, "{}:{} {} " fmt,                                   \
                              file, line, func, __VA_ARGS__);                                  \
                    break;                                                                     \
                                                                                               \
                case ::pubsub_itc_fw::LogLevel::Warning:                                       \
                    LOG_WARNING(quill_logger, "{}:{} {} " fmt,                                 \
                                file, line, func, __VA_ARGS__);                                \
                    break;                                                                     \
                                                                                               \
                case ::pubsub_itc_fw::LogLevel::Notice:                                        \
                case ::pubsub_itc_fw::LogLevel::Info:                                          \
                    LOG_INFO(quill_logger, "{}:{} {} " fmt,                                    \
                             file, line, func, __VA_ARGS__);                                   \
                    break;                                                                     \
                                                                                               \
                case ::pubsub_itc_fw::LogLevel::Debug:                                         \
                default:                                                                       \
                    LOG_DEBUG(quill_logger, "{}:{} {} " fmt,                                   \
                               file, line, func, __VA_ARGS__);                                 \
                    break;                                                                     \
            }                                                                                  \
        }                                                                                      \
    } while (false)

#define PUBSUB_LOG_STR(logger, log_level, log_string) PUBSUB_LOG(logger, log_level, "{}", log_string);

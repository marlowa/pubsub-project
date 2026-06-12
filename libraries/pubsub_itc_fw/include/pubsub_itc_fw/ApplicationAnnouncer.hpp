#pragma once
#include <pubsub_itc_fw/BuildInfo.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <unistd.h>
#include <cstring>
#include <string>

namespace pubsub_itc_fw {

class ApplicationAnnouncer {
public:
    static void announce(QuillLogger& logger, const std::string& app_name) {
        char hostname[256] = {};
        if (gethostname(hostname, sizeof(hostname) - 1) != 0) {
            std::strncpy(hostname, "unknown", sizeof(hostname) - 1);
        }
        PUBSUB_LOG(logger, FwLogLevel::Info,
            "{}: version={} pid={} built={} branch={} sha={} host={}",
            app_name,
            BuildInfo::version,
            static_cast<int>(getpid()),
            BuildInfo::build_datetime,
            BuildInfo::git_branch,
            BuildInfo::git_sha,
            hostname);
    }
};

} // namespaces

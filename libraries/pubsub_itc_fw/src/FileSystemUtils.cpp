// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/utils/FileSystemUtils.hpp>

#include <cerrno>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>

#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

std::string FileSystemUtils::make_directories(const std::string& path)
{
    if (path.empty()) {
        return "";
    }

    std::string current;
    current.reserve(path.size());

    for (std::size_t i = 0; i <= path.size(); ++i) {
        const char ch = (i < path.size()) ? path[i] : '\0';

        if (ch == '/' || ch == '\0') {
            if (current.empty() || current == "/") {
                if (ch == '/') {
                    current += ch;
                }
                continue;
            }

            if (::mkdir(current.c_str(), 0755) != 0) {
                if (errno != EEXIST) {
                    return "FileSystemUtils::make_directories: failed to create '"
                        + current + "': " + StringUtils::get_error_string(errno);
                }
            }

            if (ch == '/') {
                current += ch;
            }
        } else {
            current += ch;
        }
    }

    return "";
}

} // namespace pubsub_itc_fw

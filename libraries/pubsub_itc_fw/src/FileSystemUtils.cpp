// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/FileSystemUtils.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <cstdlib>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

std::string FileSystemUtils::make_directories(const std::string& path) {
    if (path.empty()) {
        return "";
    }

    std::string current;
    current.reserve(path.size());

    for (size_t i = 0; i <= path.size(); ++i) {
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
                    return "FileSystemUtils::make_directories: failed to create '" + current + "': " + StringUtils::get_error_string(errno);
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

std::string FileSystemUtils::mount_options(const std::string& path) {
    if (path.empty()) {
        return "";
    }

    // Resolve first, so that a relative path or one reached through a symbolic link is matched
    // against the mount it really falls under rather than the one its spelling suggests.
    std::string resolved = path;
    if (char* real = ::realpath(path.c_str(), nullptr); real != nullptr) {
        resolved = real;
        ::free(real);
    }

    std::ifstream mounts("/proc/self/mounts");
    if (!mounts) {
        return "";
    }

    // Mount points nest: a path under a mounted subdirectory also matches the root mount it
    // sits inside. So the filesystem a file lives on is the one whose mount point is the
    // LONGEST prefix of the path, not the first that matches.
    std::string best_point;
    std::string best_options;
    std::string line;
    while (std::getline(mounts, line)) {
        // device mount_point type options dump pass
        std::vector<std::string> fields;
        size_t start = 0;
        while (start <= line.size() && fields.size() < 4) {
            const size_t space = line.find(' ', start);
            const size_t end = (space == std::string::npos) ? line.size() : space;
            fields.push_back(line.substr(start, end - start));
            if (space == std::string::npos) {
                break;
            }
            start = space + 1;
        }
        if (fields.size() < 4) {
            continue;
        }

        const std::string& point = fields[1];
        if (resolved.compare(0, point.size(), point) != 0) {
            continue;
        }
        // A mount point matches only at a path boundary, so "/data/one" must not be taken to
        // cover "/data/onetwo". "/" is its own boundary.
        if (point != "/" && resolved.size() > point.size() && resolved[point.size()] != '/') {
            continue;
        }
        if (point.size() >= best_point.size()) {
            best_point = point;
            best_options = fields[3];
        }
    }

    return best_options;
}

} // namespaces

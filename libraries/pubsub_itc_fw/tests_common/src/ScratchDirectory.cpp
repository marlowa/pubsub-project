// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace pubsub_itc_fw::tests_common {

namespace {

// The base under which every scratch directory is made. Resolved once per process: the answer
// cannot change during a run, and resolving it repeatedly would let two tests in one binary
// disagree about where they are writing.
std::filesystem::path resolve_base() {
    if (const char* configured = std::getenv("PUBSUB_TEST_SCRATCH_DIR"); configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured);
    }
    if (const char* build_dir = std::getenv("PUBSUB_BUILD_DIR"); build_dir != nullptr && *build_dir != '\0') {
        return std::filesystem::path(build_dir) / "test_scratch";
    }
    return std::filesystem::current_path() / "test_scratch";
}

const std::filesystem::path& base_directory() {
    static const std::filesystem::path base = resolve_base();
    return base;
}

} // un-named namespace

std::string make_scratch_directory(const std::string& name) {
    const std::filesystem::path base = base_directory();

    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    if (ec) {
        throw std::runtime_error("make_scratch_directory: cannot create " + base.string() + ": " + ec.message());
    }

    // mkdtemp() rather than a name built from the test's own: two tests may share a name, and a
    // run left behind by a crash must not be adopted by the next run as though it were empty.
    std::string tmpl = (base / (name + "_XXXXXX")).string();
    std::vector<char> buffer(tmpl.begin(), tmpl.end());
    buffer.push_back('\0');

    if (::mkdtemp(buffer.data()) == nullptr) {
        throw std::runtime_error("make_scratch_directory: mkdtemp(" + tmpl + ") failed");
    }
    return std::string(buffer.data());
}

void remove_scratch_directory(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

std::string scratch_path_that_does_not_exist(const std::string& name) {
    const std::filesystem::path base = base_directory();

    std::error_code ec;
    std::filesystem::create_directories(base, ec);

    // Named so that it is obvious in a failure message that absence is the point, and made
    // unique so a leftover from an earlier run cannot make the path exist after all.
    const std::filesystem::path candidate = base / ("absent_" + name + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(candidate, ec);
    return candidate.string();
}

} // namespaces

#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

namespace pubsub_itc_fw {

/** @ingroup utilities_subsystem */

/**
 * @brief File open mode for log file handling.
 */
class FileOpenMode {
public:
    /**
     * @brief C-style enumeration of file open modes.
     */
    enum FileOpenModeTag {
        Append,
        Truncate
    };

    /**
     * @brief Constructs FileOpenMode from a tag value.
     * @param[in] tag File open mode tag.
     */
    constexpr explicit FileOpenMode(FileOpenModeTag tag) : mode_(tag) {}

    /**
     * @brief Returns the underlying tag value.
     */
    [[nodiscard]] FileOpenModeTag as_tag() const { return mode_; }

    /**
     * @brief Returns a string representation of the file open mode.
     * @return File open mode as string.
     */
    [[nodiscard]] std::string as_string() const {
        if (mode_ == Append)   return "Append";
        if (mode_ == Truncate) return "Truncate";
        return "unknown";
    }

    /**
     * @brief Checks equality with another FileOpenMode.
     * @param[in] rhs FileOpenMode to compare with.
     * @return True if equal, false otherwise.
     */
    [[nodiscard]] bool is_equal(const FileOpenMode& rhs) const {
        return mode_ == rhs.mode_;
    }

private:
    FileOpenModeTag mode_{Truncate};
};

inline bool operator==(const FileOpenMode& lhs, const FileOpenMode& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator!=(const FileOpenMode& lhs, const FileOpenMode& rhs) {
    return !(lhs == rhs);
}

} // namespace pubsub_itc_fw

#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.

#include <string>
#include <fmt/format.h>

namespace pubsub_itc_fw {

/** @ingroup utilities_subsystem */

/**
 * @brief File open mode for log file handling.
 */
class FileOpenMode
{
public:
    /**
     * @brief C-style enumeration of file open modes.
     */
    enum FileOpenModeTag
    {
        Append,
        Truncate
    };

private:
    FileOpenModeTag mode_;

public:
    /**
     * @brief Constructs FileOpenMode from tag value.
     *
     * TODO not sure about making these ctor explicit. The jury is out.
     *
     * @param tag File open mode tag
     */
    constexpr explicit FileOpenMode(FileOpenModeTag tag)
        : mode_{tag}
    {
    }

    /**
     * @brief Returns string representation of the file open mode.
     * @return File open mode as string
     */
    [[nodiscard]] std::string as_string() const
    {
        if (mode_ == Append) {
            return "Append";
        } else if (mode_ == Truncate) {
            return "Truncate";
        } else {
            return fmt::format("unknown ({})", static_cast<int>(mode_));
        }
    }

    /**
     * @brief Checks equality with another FileOpenMode.
     * @param rhs FileOpenMode to compare with
     * @return True if equal, false otherwise
     */
    bool is_equal(const FileOpenMode& rhs) const
    {
        return mode_ == rhs.mode_;
    }

    bool is_equal(const FileOpenModeTag& rhs) const
    {
        return mode_ == rhs;
    }

};

/**
 * @brief Equality operator for FileOpenMode.
 * @param[in] lhs Left-hand side FileOpenMode
 * @param[in] rhs Right-hand side FileOpenMode
 * @return True if equal, false otherwise
 */
inline bool operator==(const FileOpenMode& lhs, const FileOpenMode& rhs)
{
    return lhs.is_equal(rhs);
}

inline bool operator==(const FileOpenMode& lhs, const FileOpenMode::FileOpenModeTag& rhs)
{
    return lhs.is_equal(rhs);
}

inline bool operator==(const FileOpenMode::FileOpenModeTag& lhs, const FileOpenMode& rhs)
{
    return rhs.is_equal(lhs);
}

} // namespace pubsub_itc_fw

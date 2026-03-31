#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
    @brief Provides a class for high-performance, crash-resilient state persistence.
    This class encapsulates the platform-specific details of creating, mapping, and unmapping a file to memory, providing a simple, fast, and durable storage solution for an ApplicationThread's state.
*/

#include <string>
#include <vector>

namespace pubsub_itc_fw {

/**
    @brief Manages a memory-mapped file for durable state persistence.
    This class provides a direct, low-latency interface to a file on disk
    by mapping it into the application's address space. It's intended for
    storing and retrieving the application's state to enable crash recovery.

    @tparam StateType The type of the state to be stored.
*/
    template <typename StateType>
    class MemoryMappedFile  {
    public:
    /*
        The destructor ensures that the memory-mapped file is unmapped and the file handle is closed, guaranteeing that all data is flushed to disk and resources are released.
    */
    ~MemoryMappedFile();

    /**
        @brief Constructs and maps a file to memory.
        This constructor attempts to open an existing file or create a new one at the specified path. It then maps the file into memory, allowing direct access to the state data.
        @param [in] file_path The path to the state file.
    */
    explicit MemoryMappedFile(const std::string& file_path);

    /**
        @brief Provides a reference to the mapped state data.
        This allows the application to directly read and modify the
        state without the overhead of file I/O operations.
        @returns A reference to the mapped StateType object.
    */
        [[nodiscard]] StateType& get_state();

    /**
        @brief Checks if the file was created or loaded for the first time.
        This is useful for initializing the state on the first run of the
        application.
        @returns true if the file was just created, false otherwise.
        */
        [[nodiscard]] bool is_new_file() const;

private:
std::string file_path_;
int file_descriptor_;
void* mapped_address_;
bool is_new_;
};

} // namespace pubsub_itc_fw

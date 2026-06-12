// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include <pubsub_itc_fw/CpuPinning.hpp>
#include <pubsub_itc_fw/CpuRegistry.hpp>
#include <pubsub_itc_fw/FileLock.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

CpuRegistry::CpuRegistry(std::string shm_path, std::string lock_file_path)
    : shm_path_(std::move(shm_path)), lock_file_path_(std::move(lock_file_path)), my_pid_(::getpid()) {
    shm_fd_ = ::open(shm_path_.c_str(), O_CREAT | O_RDWR, 0666); // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (shm_fd_ < 0) {
        throw PubSubItcException("CpuRegistry: open('" + shm_path_ + "') failed: " + StringUtils::get_errno_string());
    }

    // Grow the file to hold exactly one SharedCoreRegistryLayout if it is smaller
    // (e.g. newly created at size 0). ftruncate zero-initialises any new bytes,
    // giving active_entry_count == 0 on first use. An existing file is unchanged.
    struct stat st {};
    if (::fstat(shm_fd_, &st) < 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
        throw PubSubItcException("CpuRegistry: fstat failed: " + StringUtils::get_errno_string());
    }

    if (st.st_size < static_cast<off_t>(sizeof(SharedCoreRegistryLayout))) {
        if (::ftruncate(shm_fd_, static_cast<off_t>(sizeof(SharedCoreRegistryLayout))) < 0) {
            ::close(shm_fd_);
            shm_fd_ = -1;
            throw PubSubItcException("CpuRegistry: ftruncate failed: " + StringUtils::get_errno_string());
        }
    }

    void* ptr = ::mmap(nullptr, sizeof(SharedCoreRegistryLayout), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (ptr == MAP_FAILED) { // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
        ::close(shm_fd_);
        shm_fd_ = -1;
        throw PubSubItcException("CpuRegistry: mmap failed: " + StringUtils::get_errno_string());
    }

    layout_ = static_cast<SharedCoreRegistryLayout*>(ptr);
}

CpuRegistry::~CpuRegistry() {
    release_cpus();
    close_mapping();
}

CpuRegistry::CpuRegistry(CpuRegistry&& other)
    : shm_path_(std::move(other.shm_path_))
    , lock_file_path_(std::move(other.lock_file_path_))
    , my_pid_(other.my_pid_)
    , layout_(other.layout_)
    , shm_fd_(other.shm_fd_) {
    other.layout_ = nullptr;
    other.shm_fd_ = -1;
}

CpuRegistry& CpuRegistry::operator=(CpuRegistry&& other) {
    if (this != &other) {
        release_cpus();
        close_mapping();
        shm_path_ = std::move(other.shm_path_);
        lock_file_path_ = std::move(other.lock_file_path_);
        my_pid_ = other.my_pid_;
        layout_ = other.layout_;
        shm_fd_ = other.shm_fd_;
        other.layout_ = nullptr;
        other.shm_fd_ = -1;
    }
    return *this;
}

AvailableCpuVector CpuRegistry::claim_cpus(size_t count, bool reserve_cpu0) const {
    if (layout_ == nullptr || count == 0) {
        return {};
    }

    const FileLock lock(lock_file_path_);

    // Compact stale entries (dead or invalid PIDs) before consulting the registry.
    // Entries with pid <= 0 are corrupt/zero-initialised and can never be released
    // by release_cpus() (which only removes entries matching the current process),
    // so they must be evicted here to prevent the table from filling permanently.
    uint32_t write_idx = 0;
    for (uint32_t i = 0; i < layout_->active_entry_count; ++i) {
        const auto& e = layout_->entries[i];
        const bool keep = (e.process_id > 0) && (kill(e.process_id, 0) == 0 || errno != ESRCH);
        if (keep) {
            layout_->entries[write_idx++] = e;
        }
    }
    layout_->active_entry_count = write_idx;

    // Find CPUs not owned by any live process.
    AvailableCpuVector available = get_available_cpu_ids(reserve_cpu0, *layout_);

    // Limit to what was requested.
    if (available.size() > count) {
        available.resize(count);
    }

    // Claim each selected CPU by writing an entry into the shared registry.
    for (const CpuAssignment& cpu : available) {
        if (layout_->active_entry_count >= SharedCoreRegistryLayout::max_system_cores) {
            // Registry full — should not happen on a well-configured system.
            break;
        }
        auto& entry = layout_->entries[layout_->active_entry_count];
        entry.core_id = cpu.cpu_id.get_value();
        entry.numa_node_id = cpu.numa_node_id;
        entry.process_id = my_pid_;
        entry.thread_tag = 0;
        entry.timestamp_ns = 0;
        ++layout_->active_entry_count;
    }

    return available;
}

void CpuRegistry::release_cpus() const {
    if (layout_ == nullptr || lock_file_path_.empty()) {
        return;
    }

    const FileLock lock(lock_file_path_);

    // Compact: shift surviving entries left over the ones we own.
    uint32_t write_idx = 0;
    for (uint32_t read_idx = 0; read_idx < layout_->active_entry_count; ++read_idx) {
        if (layout_->entries[read_idx].process_id != my_pid_) {
            layout_->entries[write_idx++] = layout_->entries[read_idx];
        }
    }
    layout_->active_entry_count = write_idx;
}

void CpuRegistry::close_mapping() {
    if (layout_ != nullptr) {
        ::munmap(layout_, sizeof(SharedCoreRegistryLayout));
        layout_ = nullptr;
    }
    if (shm_fd_ >= 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
    }
}

} // namespace pubsub_itc_fw

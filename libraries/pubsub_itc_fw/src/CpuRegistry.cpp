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
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

CpuRegistry::CpuRegistry(std::string shm_path, std::string lock_file_path)
    : shm_path_(std::move(shm_path)), lock_file_path_(std::move(lock_file_path)), my_pid_(::getpid()) {
    // Empty here would silently open the process working directory, so the
    // registry would appear to work while coordinating nothing.
    if (shm_path_.empty()) {
        throw PreconditionAssertion("CpuRegistry: shm_path must not be empty", __FILE__, __LINE__);
    }
    if (lock_file_path_.empty()) {
        throw PreconditionAssertion("CpuRegistry: lock_file_path must not be empty", __FILE__, __LINE__);
    }

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

std::tuple<bool, std::string> CpuRegistry::record_assignment(const std::vector<CpuId>& core_ids) const {
    if (layout_ == nullptr || core_ids.empty()) {
        return {true, ""};
    }

    const FileLock lock(lock_file_path_);

    // Evict entries whose owner is gone before consulting the registry, or a
    // crashed predecessor would look like a live collision for ever. Entries
    // with pid <= 0 are corrupt or zero-initialised and can never be removed by
    // release_cpus(), which only matches the current process, so they have to be
    // dropped here to stop the table filling permanently.
    uint32_t write_index = 0;
    for (uint32_t i = 0; i < layout_->active_entry_count; ++i) {
        const auto& entry = layout_->entries[i];
        const bool owner_alive = (entry.process_id > 0) && (kill(entry.process_id, 0) == 0 || errno != ESRCH);
        if (owner_alive) {
            layout_->entries[write_index++] = entry;
        }
    }
    layout_->active_entry_count = write_index;

    std::string collisions;
    for (const CpuId core_id : core_ids) {
        for (uint32_t i = 0; i < layout_->active_entry_count; ++i) {
            const auto& entry = layout_->entries[i];
            if (entry.core_id != core_id.get_value() || entry.process_id == my_pid_) {
                continue;
            }
            if (!collisions.empty()) {
                collisions += "; ";
            }
            collisions += "CPU " + std::to_string(core_id.get_value()) + " is already held by pid " + std::to_string(entry.process_id);
            break;
        }
    }

    // Record regardless of collision, so the registry stays a complete picture
    // of what is really pinned where -- which is what the affinity audit checks
    // against, and what makes a collision visible from either side.
    for (const CpuId core_id : core_ids) {
        if (layout_->active_entry_count >= SharedCoreRegistryLayout::max_system_cores) {
            break;
        }
        auto& entry = layout_->entries[layout_->active_entry_count];
        entry.core_id = core_id.get_value();
        entry.numa_node_id = -1;
        entry.process_id = my_pid_;
        entry.thread_tag = 0;
        entry.timestamp_ns = 0;
        ++layout_->active_entry_count;
    }

    if (!collisions.empty()) {
        return {false, "CPU core collision -- another installation on this machine has pinned the same core(s): " + collisions +
                           ". Both layouts are self-consistent but they overlap; the deployments must be given disjoint cores"};
    }
    return {true, ""};
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

} // namespaces

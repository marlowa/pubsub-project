#include <cstdint>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

#include <pubsub_itc_fw/MirroredBuffer.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

MirroredBuffer::~MirroredBuffer() {
    if (base_ptr_ != nullptr && base_ptr_ != MAP_FAILED) {
        munmap(base_ptr_, 2 * capacity_);
    }
    if (shm_fd_ != -1) {
        close(shm_fd_);
    }
}

MirroredBuffer::MirroredBuffer(int64_t requested_capacity) {
    if (requested_capacity <= 0) {
        throw PreconditionAssertion("Requested capacity must be greater than zero", __FILE__, __LINE__);
    }

    capacity_ = round_to_page_size(requested_capacity);

    // Create an anonymous shared memory object using memfd_create.
    // This avoids needing a named file in /dev/shm.
    shm_fd_ = memfd_create("mirrored_buffer", MFD_CLOEXEC);
    if (shm_fd_ == -1) {
        PubSubItcException::throwErrno("Failed to create memfd for mirrored buffer", __FILE__, __LINE__);
    }

    if (ftruncate(shm_fd_, capacity_) == -1) {
        PubSubItcException::throwErrno("Failed to truncate memfd to capacity", __FILE__, __LINE__);
    }

    // 1. Reserve a contiguous virtual address space of size 2 * capacity.
    // MAP_PRIVATE | MAP_ANONYMOUS with PROT_NONE ensures we don't commit physical memory yet.
    base_ptr_ = static_cast<uint8_t*>(mmap(nullptr, 2 * capacity_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (base_ptr_ == MAP_FAILED) {
        PubSubItcException::throwErrno("Failed to reserve virtual address space for mirrored buffer", __FILE__, __LINE__);
    }

    // 2. Map the physical memory into the first half of the reserved range.
    auto* first_half = static_cast<uint8_t*>(mmap(base_ptr_, capacity_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shm_fd_, 0));
    if (first_half == MAP_FAILED) {
        PubSubItcException::throwErrno("Failed to map first half of mirrored buffer", __FILE__, __LINE__);
    }

    // 3. Map the SAME physical memory into the second half.
    auto* second_half = static_cast<uint8_t*>(mmap(base_ptr_ + capacity_, capacity_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shm_fd_, 0));
    if (second_half == MAP_FAILED) {
        PubSubItcException::throwErrno("Failed to map second half of mirrored buffer", __FILE__, __LINE__);
    }
}

int64_t MirroredBuffer::capacity() const {
    return capacity_;
}

uint8_t* MirroredBuffer::write_ptr() {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return base_ptr_ + head_;
}

void MirroredBuffer::advance_head(int64_t bytes) {
    if (bytes < 0) {
        throw PreconditionAssertion("Cannot advance head by negative bytes", __FILE__, __LINE__);
    }
    if (bytes > space_remaining()) {
        throw PreconditionAssertion("Advance head exceeds available space in mirrored buffer", __FILE__, __LINE__);
    }

    head_ += bytes;
    if (head_ >= capacity_) {
        head_ -= capacity_;
    }
}

const uint8_t* MirroredBuffer::read_ptr() const {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return base_ptr_ + tail_;
}

void MirroredBuffer::advance_tail(int64_t bytes) {
    if (bytes < 0) {
        throw PreconditionAssertion("Cannot advance tail by negative bytes", __FILE__, __LINE__);
    }
    if (bytes > bytes_available()) {
        throw PreconditionAssertion("Advance tail exceeds available data in mirrored buffer", __FILE__, __LINE__);
    }

    tail_ += bytes;
    if (tail_ >= capacity_) {
        tail_ -= capacity_;
    }
}

int64_t MirroredBuffer::bytes_available() const {
    const int64_t diff = head_ - tail_;
    return (diff >= 0) ? diff : (capacity_ + diff);
}

int64_t MirroredBuffer::space_remaining() const {
    // We leave one byte unused to distinguish between the buffer being
    // completely empty (head == tail) and completely full.
    return capacity_ - bytes_available() - 1;
}

/*static*/ int64_t MirroredBuffer::round_to_page_size(int64_t size) {
    const int64_t page_size_raw = sysconf(_SC_PAGESIZE);
    if (page_size_raw <= 0) {
        PubSubItcException::throwErrno("Failed to retrieve system page size", __FILE__, __LINE__);
    }

    const auto u_size = static_cast<uint64_t>(size);
    const auto u_page_size = static_cast<uint64_t>(page_size_raw);

    // Perform bitwise logic on unsigned types to satisfy hicpp-signed-bitwise
    const uint64_t aligned = (u_size + u_page_size - 1U) & ~(u_page_size - 1U);
    return static_cast<int64_t>(aligned);
}

} // namespace pubsub_itc_fw

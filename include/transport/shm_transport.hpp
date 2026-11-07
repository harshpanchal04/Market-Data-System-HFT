#pragma once

/**
 * @file shm_transport.hpp
 * @brief POSIX shared-memory transport layer (RAII wrapper).
 *
 * Wraps shm_open / ftruncate / mmap into a single RAII class that owns
 * its file descriptor and mapped region. Supports both creator (writer)
 * and consumer (reader) roles with appropriate protection flags.
 *
 * POSIX shared memory is chosen over System-V shmget because:
 *  - Named segments are visible in the filesystem under /dev/shm (Linux),
 *    making debugging with standard tools trivial.
 *  - shm_open follows the same open(2) semantics familiar to POSIX programmers.
 *  - RAII cleanup via shm_unlink ensures no segments linger after the process
 *    exits unexpectedly.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <string>

namespace mds {

/**
 * @class ShmTransport
 * @brief RAII owner of a POSIX shared-memory segment.
 *
 * Roles:
 *  - **Creator** (`create = true`): calls shm_open(O_CREAT|O_RDWR), sizes the
 *    segment with ftruncate, then maps it PROT_READ|PROT_WRITE.
 *    Calls shm_unlink on destruction to remove the name from the VFS.
 *  - **Consumer** (`create = false`): opens an existing segment O_RDWR and
 *    maps it PROT_READ|PROT_WRITE. The SPSC consumer must write read_idx_,
 *    which lives in the segment, so a read-only mapping faults on the first
 *    pop(). Does *not* unlink on destruction.
 *
 * Example (creator):
 * @code
 *   ShmTransport shm("mds_ticks", sizeof(SpscQueue), true);
 *   auto* queue = new (shm.ptr<void>()) SpscQueue{};
 * @endcode
 *
 * Example (consumer):
 * @code
 *   ShmTransport shm("mds_ticks", sizeof(SpscQueue), false);
 *   auto* queue = shm.ptr<SpscQueue>();
 * @endcode
 */
class ShmTransport {
public:
    /**
     * @param name    Segment name, without a leading slash (e.g. "mds_ticks").
     * @param size    Desired size in bytes.
     * @param create  true → creator role; false → consumer (attach) role.
     * @throws std::runtime_error on any POSIX API failure.
     */
    ShmTransport(const std::string& name, std::size_t size, bool create)
        : size_(size), name_("/" + name), is_creator_(create)
    {
        if (name.empty())  throw std::runtime_error("ShmTransport: segment name is empty");
        if (size == 0)     throw std::runtime_error("ShmTransport: segment size is zero");

        int oflag = create ? (O_CREAT | O_RDWR) : O_RDWR;
        fd_       = ::shm_open(name_.c_str(), oflag, 0666);
        if (fd_ == -1) {
            throw std::runtime_error("ShmTransport: shm_open failed for '" + name_ + "'");
        }

        if (create) {
            struct ::stat st{};
            if (::fstat(fd_, &st) == -1) {
                ::close(fd_);
                ::shm_unlink(name_.c_str());
                throw std::runtime_error("ShmTransport: fstat failed");
            }
            // Only truncate if newly created (size == 0); reuse existing size otherwise.
            if (st.st_size == 0) {
                if (::ftruncate(fd_, static_cast<off_t>(size_)) == -1) {
                    ::close(fd_);
                    ::shm_unlink(name_.c_str());
                    throw std::runtime_error("ShmTransport: ftruncate failed");
                }
            }
        }

        int prot   = PROT_READ | PROT_WRITE; // both roles write their own index
        addr_      = ::mmap(nullptr, size_, prot, MAP_SHARED, fd_, 0);
        if (addr_ == MAP_FAILED) {
            ::close(fd_);
            if (create) ::shm_unlink(name_.c_str());
            throw std::runtime_error("ShmTransport: mmap failed");
        }
    }

    ~ShmTransport() { destroy(); }

    // Non-copyable
    ShmTransport(const ShmTransport&)            = delete;
    ShmTransport& operator=(const ShmTransport&) = delete;

    // Move-constructible
    ShmTransport(ShmTransport&& o) noexcept
        : fd_(o.fd_), addr_(o.addr_), size_(o.size_),
          name_(std::move(o.name_)), is_creator_(o.is_creator_)
    {
        o.fd_         = -1;
        o.addr_       = MAP_FAILED;
        o.is_creator_ = false;
    }

    ShmTransport& operator=(ShmTransport&& o) noexcept {
        if (this != &o) {
            destroy();
            fd_         = o.fd_;
            addr_       = o.addr_;
            size_       = o.size_;
            name_       = std::move(o.name_);
            is_creator_ = o.is_creator_;
            o.fd_       = -1;
            o.addr_     = MAP_FAILED;
            o.is_creator_ = false;
        }
        return *this;
    }

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------

    /// Returns a typed pointer to the mapped region.
    template <typename T = void>
    T* ptr() const noexcept {
        return static_cast<T*>(addr_);
    }

    [[nodiscard]] bool        valid()      const noexcept { return addr_ != MAP_FAILED && fd_ != -1; }
    [[nodiscard]] std::size_t size()       const noexcept { return size_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool        is_creator() const noexcept { return is_creator_; }

private:
    void destroy() noexcept {
        if (addr_ != MAP_FAILED) {
            ::munmap(addr_, size_);
            addr_ = MAP_FAILED;
        }
        if (fd_ != -1) {
            ::close(fd_);
            fd_ = -1;
        }
        if (is_creator_ && !name_.empty()) {
            ::shm_unlink(name_.c_str());
            is_creator_ = false;
        }
    }

    int         fd_         = -1;
    void*       addr_       = MAP_FAILED;
    std::size_t size_       = 0;
    std::string name_;
    bool        is_creator_ = false;
};

} // namespace mds

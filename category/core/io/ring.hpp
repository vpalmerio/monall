// Copyright (C) 2025 Category Labs, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

#include <category/core/io/config.hpp>

#ifdef _WIN32
    // <ioringapi.h> declares BuildIoRingWriteFile etc. in terms of
    // FILE_WRITE_FLAGS/FILE_FLUSH_MODE, which it assumes are already visible
    // from <winbase.h> (it only pulls in <minwinbase.h> itself).
    #include <windows.h>

    #include <ioringapi.h>
#else
    #include <liburing.h>
#endif

#include <optional>

MONAD_IO_NAMESPACE_BEGIN

struct RingConfig
{
    unsigned entries{128}; //!< Number of submission queue entries
    //! If set, turn on kernel polling of submission ring on the specified CPU
    std::optional<unsigned> sq_thread_cpu{};

    RingConfig() = default;

    constexpr explicit RingConfig(unsigned const entries_)
        : entries(entries_)
    {
    }

    constexpr RingConfig(
        unsigned const entries_, std::optional<unsigned> const sq_thread_cpu_)
        : entries(entries_)
        , sq_thread_cpu(sq_thread_cpu_)
    {
    }
};

class Ring final
{
#ifdef _WIN32
    HIORING ring_{nullptr};
    RingConfig const config_;
#else
    io_uring ring_;
    io_uring_params const params_;
#endif

public:
    explicit Ring(RingConfig const &config = {});
    ~Ring();

    Ring(Ring const &) = delete;
    Ring(Ring &&) = delete;
    Ring &operator=(Ring const &) = delete;
    Ring &operator=(Ring &&) = delete;

#ifdef _WIN32
    // Distinct name from Linux's get_ring()/get_params(): the two are never
    // compiled in the same TU (no ODR concern), but a distinct name keeps
    // call sites in io_windows.cpp/buffers.cpp self-documenting about which
    // API surface they're using.
    [[gnu::always_inline]] HIORING get_handle() const
    {
        return ring_;
    }
#else
    [[gnu::always_inline]] io_uring const &get_ring() const
    {
        return ring_;
    }

    [[gnu::always_inline]] io_uring &get_ring()
    {
        return ring_;
    }

    [[gnu::always_inline]] io_uring_params const &get_params() const
    {
        return params_;
    }
#endif

    [[gnu::always_inline]] unsigned get_sq_entries() const
    {
#ifdef _WIN32
        return config_.entries;
#else
        return params_.sq_entries;
#endif
    }

    [[gnu::always_inline]] unsigned get_cq_entries() const
    {
#ifdef _WIN32
        return config_.entries;
#else
        return params_.cq_entries;
#endif
    }

    [[gnu::always_inline]] bool must_call_uring_submit() const
    {
#ifdef _WIN32
        // IoRing has no SQPOLL-equivalent kernel-polling thread: userspace
        // must always call SubmitIoRing to make queued entries visible.
        return true;
#else
        return !(params_.flags & IORING_SETUP_SQPOLL);
#endif
    }
};

#ifndef _WIN32
static_assert(sizeof(Ring) == 336);
static_assert(alignof(Ring) == 8);
#endif

MONAD_IO_NAMESPACE_END

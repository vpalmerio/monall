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

#include <category/core/io/ring.hpp>

#include <category/core/io/config.hpp>

#include <category/core/assert.h>

#ifdef _WIN32

MONAD_IO_NAMESPACE_BEGIN

// Request IORING_VERSION_3: BuildIoRingWriteFile (and
// IOSQE_FLAGS_DRAIN_PRECEDING_OPS, used by the write path to order a write
// after preceding reads on a shared ring) return CO_E_NOT_SUPPORTED on a
// ring created with IORING_VERSION_2 -- writes are a V3 feature.
//
// sq_thread_cpu is ignored: IoRing has no kernel-thread-polling
// configuration analogous to io_uring's IORING_SETUP_SQPOLL/SQ_AFF.
Ring::Ring(RingConfig const &config)
    : config_{config}
{
    HRESULT const hr = CreateIoRing(
        IORING_VERSION_3,
        {IORING_CREATE_REQUIRED_FLAGS_NONE, IORING_CREATE_ADVISORY_FLAGS_NONE},
        config.entries,
        config.entries,
        &ring_);
    MONAD_ASSERT_PRINTF(
        SUCCEEDED(hr), "CreateIoRing failed with HRESULT 0x%08lx", hr);
}

Ring::~Ring()
{
    if (ring_ != nullptr) {
        CloseIoRing(ring_);
    }
}

MONAD_IO_NAMESPACE_END

#else

    #include <cstring>

    #include <liburing.h>
    #include <liburing/io_uring.h>
    #include <string.h>

MONAD_IO_NAMESPACE_BEGIN

Ring::Ring(RingConfig const &config)
    : ring_{}
    , params_{[&] {
        io_uring_params ret;
        std::memset(&ret, 0, sizeof(ret));
        if (config.sq_thread_cpu) {
            ret.flags |= IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
            ret.sq_thread_cpu = *config.sq_thread_cpu;
            ret.sq_thread_idle = 60 * 1000;
        }
        return ret;
    }()}
{
    int const result = io_uring_queue_init_params(
        config.entries, &ring_, const_cast<io_uring_params *>(&params_));
    MONAD_ASSERT_PRINTF(
        result == 0,
        "io_uring_queue_init_params failed: %s (%d)",
        strerror(-result),
        -result);
}

Ring::~Ring()
{
    io_uring_queue_exit(&ring_);
}

MONAD_IO_NAMESPACE_END

#endif

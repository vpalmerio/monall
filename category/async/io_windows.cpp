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

// Windows placeholder for AsyncIO. The constructor/destructor and plain
// bookkeeping helpers work normally, but the io_uring-based submission and
// polling paths are not yet ported -- they abort if ever reached. No Phase 2
// test constructs an AsyncIO, so these stubs are unreachable for now. See the
// Windows port plan for the follow-up IoRing-based port.

#include <category/async/io.hpp>

#include <category/async/config.hpp>
#include <category/async/detail/connected_operation_storage.hpp>
#include <category/async/erased_connected_operation.hpp>
#include <category/async/storage_pool.hpp>
#include <category/core/assert.h>
#include <category/core/io/buffers.hpp>
#include <category/core/io/ring.hpp>
#include <category/core/tl_tid.h>

#include <cstddef>
#include <filesystem>
#include <span>
#include <utility>

MONAD_ASYNC_NAMESPACE_BEGIN

namespace detail
{
    struct AsyncIO_per_thread_state_t::within_completions_holder
    {
        AsyncIO_per_thread_state_t *parent;

        explicit within_completions_holder(
            AsyncIO_per_thread_state_t *const parent_)
            : parent(parent_)
        {
            parent->within_completions_count++;
        }

        within_completions_holder(within_completions_holder const &) = delete;

        within_completions_holder(within_completions_holder &&other) noexcept
            : parent(other.parent)
        {
            other.parent = nullptr;
        }

        ~within_completions_holder()
        {
            if (parent && 0 == --parent->within_completions_count) {
                parent->within_completions_reached_zero();
            }
        }
    };

    AsyncIO_per_thread_state_t::within_completions_holder
    AsyncIO_per_thread_state_t::enter_completions()
    {
        return within_completions_holder{this};
    }

    extern __attribute__((visibility("default"))) AsyncIO_per_thread_state_t &
    AsyncIO_per_thread_state()
    {
        static thread_local AsyncIO_per_thread_state_t v;
        return v;
    }
}

AsyncIO::AsyncIO(class storage_pool &pool, monad::io::Buffers &rwbuf)
    : owning_tid_(get_tl_tid())
    , storage_pool_{std::addressof(pool)}
    , cnv_chunk_{pool.chunk(storage_pool::cnv, 0)}
    , uring_(rwbuf.ring())
    , wr_uring_(rwbuf.wr_ring())
    , rwbuf_(rwbuf)
    , rd_pool_(monad::io::BufferPool(rwbuf, true))
    , wr_pool_(monad::io::BufferPool(rwbuf, false))
{
    extant_write_operations_::init_header(&extant_write_operations_header_);

    auto &ts = detail::AsyncIO_per_thread_state();
    MONAD_ASSERT_PRINTF(
        ts.instance == nullptr,
        "currently cannot create more than one AsyncIO per thread at a time");
    ts.instance = this;

    auto const count = pool.chunks(storage_pool::seq);
    for (size_t n = 0; n < count; n++) {
        seq_chunks_.emplace_back(
            pool.chunk(storage_pool::seq, static_cast<uint32_t>(n)));
    }
    // The io_uring submission/polling rings are not yet ported to Windows
    // (IoRing), so registration of chunk file descriptors with the rings is
    // skipped here.
}

AsyncIO::~AsyncIO()
{
    try {
        wait_until_done();
    }
    catch (...) {
        std::terminate();
    }

    auto &ts = detail::AsyncIO_per_thread_state();
    MONAD_ASSERT_PRINTF(
        ts.instance == this,
        "this is being destructed not from its thread, bad idea");
    ts.instance = nullptr;
}

void AsyncIO::account_read_(size_t const size)
{
    if (++records_.inflight_rd > records_.max_inflight_rd) {
        records_.max_inflight_rd = records_.inflight_rd;
    }
    ++records_.nreads;
    records_.bytes_read += size;
}

void AsyncIO::prepare_read_sqe_(
    struct io_uring_sqe *const sqe, std::span<std::byte> const buffer,
    chunk_offset_t const chunk_and_offset, void *const uring_data,
    enum erased_connected_operation::io_priority const prio)
{
    (void)sqe;
    (void)buffer;
    (void)chunk_and_offset;
    (void)uring_data;
    (void)prio;
    MONAD_ABORT_PRINTF("AsyncIO not yet implemented on Windows");
}

void AsyncIO::prepare_read_sqe_(
    struct io_uring_sqe *const sqe, std::span<const struct iovec> const buffers,
    chunk_offset_t const chunk_and_offset, void *const uring_data,
    enum erased_connected_operation::io_priority const prio)
{
    (void)sqe;
    (void)buffers;
    (void)chunk_and_offset;
    (void)uring_data;
    (void)prio;
    MONAD_ABORT_PRINTF("AsyncIO not yet implemented on Windows");
}

void AsyncIO::submit_request_sqe_(
    std::span<std::byte> const buffer, chunk_offset_t const chunk_and_offset,
    void *const uring_data,
    enum erased_connected_operation::io_priority const prio)
{
    (void)buffer;
    (void)chunk_and_offset;
    (void)uring_data;
    (void)prio;
    MONAD_ABORT_PRINTF("AsyncIO not yet implemented on Windows");
}

void AsyncIO::submit_request_(
    std::span<std::byte> const buffer, chunk_offset_t const chunk_and_offset,
    void *const uring_data,
    enum erased_connected_operation::io_priority const prio)
{
    (void)buffer;
    (void)chunk_and_offset;
    (void)uring_data;
    (void)prio;
    MONAD_ABORT_PRINTF("AsyncIO not yet implemented on Windows");
}

void AsyncIO::submit_request_(
    std::span<const struct iovec> const buffers,
    chunk_offset_t const chunk_and_offset, void *const uring_data,
    enum erased_connected_operation::io_priority const prio)
{
    (void)buffers;
    (void)chunk_and_offset;
    (void)uring_data;
    (void)prio;
    MONAD_ABORT_PRINTF("AsyncIO not yet implemented on Windows");
}

void AsyncIO::submit_request_(
    std::span<std::byte const> const buffer,
    chunk_offset_t const chunk_and_offset, void *const uring_data,
    enum erased_connected_operation::io_priority const prio)
{
    (void)buffer;
    (void)chunk_and_offset;
    (void)uring_data;
    (void)prio;
    MONAD_ABORT_PRINTF("AsyncIO not yet implemented on Windows");
}

void AsyncIO::poll_uring_while_submission_queue_full_()
{
    MONAD_ABORT_PRINTF("AsyncIO not yet implemented on Windows");
}

size_t AsyncIO::poll_uring_(bool const blocking, unsigned const poll_rings_mask)
{
    (void)blocking;
    (void)poll_rings_mask;
    MONAD_ABORT_PRINTF("AsyncIO not yet implemented on Windows");
}

unsigned AsyncIO::deferred_initiations_in_flight() const noexcept
{
    auto const &ts = detail::AsyncIO_per_thread_state();
    return !ts.empty() && !ts.am_within_completions();
}

std::pair<unsigned, unsigned>
AsyncIO::io_uring_ring_entries_left(bool const for_wr_ring) const noexcept
{
    (void)for_wr_ring;
    return {0, 0};
}

void AsyncIO::dump_fd_to(size_t const which, std::filesystem::path const &path)
{
    (void)which;
    (void)path;
    MONAD_ABORT_PRINTF("AsyncIO not yet implemented on Windows");
}

unsigned char *AsyncIO::poll_uring_while_no_io_buffers_(bool const is_write)
{
    (void)is_write;
    MONAD_ABORT_PRINTF("AsyncIO not yet implemented on Windows");
}

MONAD_ASYNC_NAMESPACE_END

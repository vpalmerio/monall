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

// Windows AsyncIO implementation built on the Win32 IoRing API
// (CreateIoRing/BuildIoRing*/SubmitIoRing/PopIoRingCompletion).
//
// Single-buffer reads and writes use IoRing (Phase 5a). The iovec scatter-read
// overload of submit_request_() falls back to synchronous pread() because
// IoRing has no IORING_OP_READV equivalent and unregistered-buffer refs cause
// E_INVALIDARG in SubmitIoRing (Phase 5b).
//
// The concurrent-read-limit deferred-SQE queue, eager-completion draining, and
// EAGAIN retry-on-reinitiate remain MONAD_ABORT_PRINTF stubs — none are
// reachable as long as concurrent_read_io_limit_ stays 0 and
// eager_completions_ stays false. See the Windows port plan for follow-up.

#include <category/async/io.hpp>

#include <category/async/config.hpp>
#include <category/async/detail/connected_operation_storage.hpp>
#include <category/async/detail/scope_polyfill.hpp>
#include <category/async/erased_connected_operation.hpp>
#include <category/async/storage_pool.hpp>
#include <category/core/assert.h>
#include <category/core/compat.h>
#include <category/core/io/buffers.hpp>
#include <category/core/io/ring.hpp>
#include <category/core/tl_tid.h>

#include <ankerl/unordered_dense.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

// MONAD_WIN_HEAP_CHECK() is defined in category/core/compat.h (included
// transitively via category/async/config.hpp → compat.h).  The macro
// validates ALL process heaps so it catches corruption in the ucrtbase CRT
// heap, not just GetProcessHeap().  It is temporary — remove with all call
// sites once the Phase 5 heap-corruption root cause is found.
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

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

namespace
{
    // Unlike Linux's io_uring_register_files (a direct syscall), IoRing's
    // BuildIoRingRegisterFileHandles only enqueues an SQE; the registration
    // doesn't take effect (and the file indices it allocates aren't usable)
    // until that SQE is submitted and its completion observed.
    void submit_and_wait_for_completion(HIORING const ring)
    {
        UINT32 submitted = 0;
        HRESULT hr = SubmitIoRing(ring, 1, INFINITE, &submitted);
        MONAD_ASSERT_PRINTF(
            SUCCEEDED(hr), "SubmitIoRing failed with HRESULT 0x%08lx", hr);
        IORING_CQE cqe;
        hr = PopIoRingCompletion(ring, &cqe);
        MONAD_ASSERT_PRINTF(
            SUCCEEDED(hr),
            "PopIoRingCompletion failed with HRESULT 0x%08lx",
            hr);
        MONAD_ASSERT_PRINTF(
            SUCCEEDED(cqe.ResultCode),
            "registration failed with HRESULT 0x%08lx",
            cqe.ResultCode);
    }

    // Coarse HRESULT -> errno mapping for i/o completion failures. This is
    // deliberately minimal for 5a; expand as real failure modes are
    // exercised by later phases.
    int hresult_to_errno_(HRESULT const hr)
    {
        switch (hr) {
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED):
            return EACCES;
        case HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE):
            return EBADF;
        case HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY):
        case HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY):
            return ENOMEM;
        default:
            return EIO;
        }
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
    if (wr_uring_ != nullptr) {
        // The write ring must have at least as many submission entries as
        // there are write i/o buffers
        auto const [sqes, cqes] = io_uring_ring_entries_left(true);
        MONAD_ASSERT_PRINTF(
            rwbuf.get_write_count() <= sqes,
            "rwbuf write count %zu sqes %u",
            rwbuf.get_write_count(),
            sqes);
    }

    auto &ts = detail::AsyncIO_per_thread_state();
    MONAD_ASSERT_PRINTF(
        ts.instance == nullptr,
        "currently cannot create more than one AsyncIO per thread at a time");
    ts.instance = this;

    auto const count = pool.chunks(storage_pool::seq);
    std::vector<int> fds;
    fds.reserve(count * 2 + 2);
    fds.push_back(cnv_chunk_.io_uring_read_fd);
    fds.push_back(cnv_chunk_.io_uring_write_fd);
    for (size_t n = 0; n < count; n++) {
        seq_chunks_.emplace_back(
            pool.chunk(storage_pool::seq, static_cast<uint32_t>(n)));
        MONAD_ASSERT_PRINTF(
            seq_chunks_.back().chunk.capacity() >= MONAD_IO_BUFFERS_WRITE_SIZE,
            "sequential chunk capacity %llu must equal or exceed i/o buffer "
            "size %zu",
            seq_chunks_.back().chunk.capacity(),
            MONAD_IO_BUFFERS_WRITE_SIZE);
        MONAD_ASSERT(
            (seq_chunks_.back().chunk.capacity() %
             MONAD_IO_BUFFERS_WRITE_SIZE) == 0);
        fds.push_back(seq_chunks_[n].io_uring_read_fd);
        fds.push_back(seq_chunks_[n].io_uring_write_fd);
    }

    /* Annoyingly IoRing, like io_uring, refuses duplicate file handles in its
    registration, and for efficiency the zoned storage emulation returns the
    same file descriptor for reads (and it may do so for writes depending). So
    reduce to a minimum mapped set.
    */
    ankerl::unordered_dense::segmented_map<int, int> fd_to_iouring_map;
    for (auto const fd : fds) {
        MONAD_ASSERT(fd != -1);
        fd_to_iouring_map[fd] = -1;
    }
    int idx = 0;
    fds.clear();
    for (auto &fd : fd_to_iouring_map) {
        fd.second = idx++;
        fds.push_back(fd.first);
    }

    // Storage pool opens all file handles with FILE_FLAG_OVERLAPPED so that
    // Windows IoRing can issue reads and writes to them (IoRing requires
    // overlapped handles for I/O to be delivered correctly). The pread/pwrite
    // shims in compat.h are updated to call GetOverlappedResult when WriteFile
    // or ReadFile returns ERROR_IO_PENDING on overlapped handles.
    std::vector<HANDLE> handles;
    handles.reserve(fds.size());
    for (auto const fd : fds) {
        HANDLE const h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        MONAD_ASSERT(h != INVALID_HANDLE_VALUE);
        handles.push_back(h);
    }

    HRESULT hr = BuildIoRingRegisterFileHandles(
        uring_.get_handle(),
        static_cast<UINT32>(handles.size()),
        handles.data(),
        0);
    MONAD_ASSERT_PRINTF(
        SUCCEEDED(hr),
        "BuildIoRingRegisterFileHandles failed with HRESULT 0x%08lx",
        hr);
    submit_and_wait_for_completion(uring_.get_handle());
    if (wr_uring_ != nullptr) {
        hr = BuildIoRingRegisterFileHandles(
            wr_uring_->get_handle(),
            static_cast<UINT32>(handles.size()),
            handles.data(),
            0);
        MONAD_ASSERT_PRINTF(
            SUCCEEDED(hr),
            "BuildIoRingRegisterFileHandles failed with HRESULT 0x%08lx",
            hr);
        submit_and_wait_for_completion(wr_uring_->get_handle());
    }

    auto replace_fds_with_iouring_fds = [&](auto &p) {
        auto it = fd_to_iouring_map.find(p.io_uring_read_fd);
        MONAD_ASSERT(it != fd_to_iouring_map.end());
        p.io_uring_read_fd = it->second;
        it = fd_to_iouring_map.find(p.io_uring_write_fd);
        MONAD_ASSERT(it != fd_to_iouring_map.end());
        p.io_uring_write_fd = it->second;
    };
    replace_fds_with_iouring_fds(cnv_chunk_);
    for (auto &chnk : seq_chunks_) {
        replace_fds_with_iouring_fds(chnk);
    }
    MONAD_WIN_HEAP_CHECK(); // constructor end: catch corruption introduced during AsyncIO init
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

    // IoRing has no unregister-file-handles operation; CloseIoRing (in
    // Ring's destructor, which runs after AsyncIO's) releases all
    // registrations at whole-ring granularity.
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
    // Only reached via the concurrent_read_io_limit_ deferred-SQE path, which
    // stays at its default of 0 (disabled) on Windows for now.
    MONAD_ABORT_PRINTF(
        "concurrent_read_io_limit_ deferred reads not yet implemented on "
        "Windows");
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
    MONAD_ABORT_PRINTF("scatter reads not yet implemented on Windows");
}

void AsyncIO::submit_request_sqe_(
    std::span<std::byte> const buffer, chunk_offset_t const chunk_and_offset,
    void *const uring_data,
    enum erased_connected_operation::io_priority const prio)
{
    // Win32 IoRing has no ioprio-equivalent SQE field.
    (void)prio;

    MONAD_ASSERT(uring_data != nullptr);
    MONAD_ASSERT((chunk_and_offset.offset & (DISK_PAGE_SIZE - 1)) == 0);
    MONAD_ASSERT(buffer.size() <= READ_BUFFER_SIZE);

#ifndef NDEBUG
    memset(buffer.data(), 0xff, buffer.size());
#endif

    auto const &ci = seq_chunks_[chunk_and_offset.id];
    auto const buf_offset = static_cast<UINT32>(
        reinterpret_cast<unsigned char *>(buffer.data()) -
        rwbuf_.get_read_buffer(0));

    HRESULT hr = BuildIoRingReadFile(
        uring_.get_handle(),
        IoRingHandleRefFromIndex(ci.io_uring_read_fd),
        IoRingBufferRefFromIndexAndOffset(
            monad::io::Buffers::get_read_index(), buf_offset),
        static_cast<UINT32>(buffer.size()),
        ci.chunk.read_fd().second + chunk_and_offset.offset,
        reinterpret_cast<UINT_PTR>(uring_data),
        IOSQE_FLAGS_NONE);
    MONAD_ASSERT_PRINTF(
        SUCCEEDED(hr), "BuildIoRingReadFile failed with HRESULT 0x%08lx", hr);

    UINT32 submitted = 0;
    hr = SubmitIoRing(uring_.get_handle(), 0, 0, &submitted);
    MONAD_ASSERT_PRINTF(
        SUCCEEDED(hr), "SubmitIoRing failed with HRESULT 0x%08lx", hr);
}

void AsyncIO::submit_request_(
    std::span<std::byte> const buffer, chunk_offset_t const chunk_and_offset,
    void *const uring_data,
    enum erased_connected_operation::io_priority const prio)
{
    poll_uring_while_submission_queue_full_();

    submit_request_sqe_(buffer, chunk_and_offset, uring_data, prio);
}

size_t AsyncIO::submit_request_(
    std::span<const struct iovec> const buffers,
    chunk_offset_t const chunk_and_offset, void *const uring_data,
    enum erased_connected_operation::io_priority const prio)
{
    // Win32 IoRing has no native scatter/gather (no IORING_OP_READV
    // equivalent). IoRingBufferRefFromPointer (RAW/unregistered kind) is
    // silently accepted by BuildIoRingReadFile but causes E_INVALIDARG in the
    // next SubmitIoRing call — so fall back to synchronous pread() for the
    // single-iovec case.
    //
    // Only the single-iovec case is needed: MPT's read_long_update_sender
    // (the only caller) always passes exactly 1 iovec (one contiguous
    // _aligned_malloc buffer for node reads larger than READ_BUFFER_SIZE).
    MONAD_ASSERT_PRINTF(
        buffers.size() == 1,
        "scatter reads with %zu iovecs not yet implemented on Windows",
        buffers.size());

    // pread() does not use uring_data for the I/O itself, but uring_data is
    // the connected-operation state we must notify later from poll_uring_();
    // suppress the now-unused prio parameter.
    (void)prio;

    auto const &iov = buffers.front();
    MONAD_ASSERT((chunk_and_offset.offset & (DISK_PAGE_SIZE - 1)) == 0);

    // Chunk fds are created via _open_osfhandle (ucrtbase); pread() from
    // ucrtbase works on the same fd table — same path dump_fd_to() uses.
    auto const &ci = seq_chunks_[chunk_and_offset.id];
    off_t const file_offset = static_cast<off_t>(
        ci.chunk.read_fd().second + chunk_and_offset.offset);
    MONAD_WIN_HEAP_CHECK();
    ssize_t const bytes = ::pread(
        ci.chunk.read_fd().first, iov.iov_base, iov.iov_len, file_offset);
    MONAD_ASSERT_PRINTF(
        bytes >= 0,
        "pread (long read fd=%d offset=%lld len=%zu) failed: %s",
        ci.chunk.read_fd().first,
        (long long)file_offset,
        iov.iov_len,
        strerror(errno));
    MONAD_WIN_HEAP_CHECK();

    // Do NOT notify uring_data->completed() here: that would run the
    // receiver's continuation reentrantly, inside the call stack that is
    // still initiating this very read (e.g. trie.cpp's npending-based
    // deferred finalization assumes a read can never complete before its
    // initiator returns -- exactly what Linux's genuinely-async io_uring
    // submission guarantees). Queue the already-known result and report
    // "pending" instead; poll_uring_() will notify it on its next call,
    // exactly like a real completion.
    sync_completed_long_reads_.emplace_back(
        static_cast<erased_connected_operation *>(uring_data),
        static_cast<size_t>(bytes < 0 ? 0 : bytes));
    return size_t(-1);
}

void AsyncIO::submit_request_(
    std::span<std::byte const> const buffer,
    chunk_offset_t const chunk_and_offset, void *const uring_data,
    enum erased_connected_operation::io_priority const prio)
{
    // Win32 IoRing has no ioprio-equivalent SQE field.
    (void)prio;

    MONAD_WIN_HEAP_CHECK();
    MONAD_ASSERT(uring_data != nullptr);
    MONAD_ASSERT(!rwbuf_.is_read_only());
    MONAD_ASSERT((chunk_and_offset.offset & (DISK_PAGE_SIZE - 1)) == 0);
    MONAD_ASSERT(buffer.size() <= WRITE_BUFFER_SIZE);

    auto const &ci = seq_chunks_[chunk_and_offset.id];
    auto const offset = ci.chunk.write_fd(buffer.size()).second;
    /* Do sanity check to ensure initiator is definitely appending where
    they are supposed to be appending.
    */
    MONAD_ASSERT_PRINTF(
        (chunk_and_offset.offset & 0xffff) == (offset & 0xffff),
        "where we are appending %u is not where we are supposed to be "
        "appending %llu. Chunk id is %u",
        (chunk_and_offset.offset & 0xffff),
        (offset & 0xffff),
        chunk_and_offset.id);

    monad::io::Ring &write_ring =
        (wr_uring_ != nullptr) ? *wr_uring_ : uring_;
    UINT32 const buf_index = (wr_uring_ != nullptr)
                                  ? 0
                                  : monad::io::Buffers::get_write_index();
    auto const buf_offset = static_cast<UINT32>(
        reinterpret_cast<unsigned char const *>(buffer.data()) -
        rwbuf_.get_write_buffer(0));
    // Mirror Linux's io.cpp: drain preceding ops only when writing to a
    // segregated write ring (wr_uring_ != nullptr). Linux's io_uring SQE
    // gets IOSQE_IO_DRAIN exactly in that case (`wr_ring !=
    // &uring_.get_ring()`), forcing each write on that ring to wait for all
    // earlier writes on the same ring to finish before it starts -- without
    // it, two writes issued back-to-back (e.g. trie.cpp's recursive
    // node-writer continuations) can complete out of submission order,
    // which AsyncIO.sqe_exhaustion_does_not_reorder_writes caught. A
    // *shared* ring isn't drained on Linux either, so this doesn't drain it
    // here.
    IORING_SQE_FLAGS const sqe_flags = (wr_uring_ != nullptr)
                                            ? IOSQE_FLAGS_DRAIN_PRECEDING_OPS
                                            : IOSQE_FLAGS_NONE;

    HRESULT hr = BuildIoRingWriteFile(
        write_ring.get_handle(),
        IoRingHandleRefFromIndex(ci.io_uring_write_fd),
        IoRingBufferRefFromIndexAndOffset(buf_index, buf_offset),
        static_cast<UINT32>(buffer.size()),
        offset,
        FILE_WRITE_FLAGS_NONE,
        reinterpret_cast<UINT_PTR>(uring_data),
        sqe_flags);
    MONAD_ASSERT_PRINTF(
        SUCCEEDED(hr), "BuildIoRingWriteFile failed with HRESULT 0x%08lx", hr);

    UINT32 submitted = 0;
    hr = SubmitIoRing(write_ring.get_handle(), 0, 0, &submitted);
    MONAD_ASSERT_PRINTF(
        SUCCEEDED(hr), "SubmitIoRing failed with HRESULT 0x%08lx", hr);
}

void AsyncIO::poll_uring_while_submission_queue_full_()
{
    // IoRing has no io_uring_sq_space_left()/io_uring_cq_ready() equivalent;
    // use the inflight-read count against the ring's configured queue depth
    // as a proxy for "is the submission queue full".
    while (records_.inflight_rd >= uring_.get_sq_entries()) {
        MONAD_ASSERT_PRINTF(
            io_in_flight() > 0,
            "read submission queue full (%u/%u) with no i/o in flight",
            records_.inflight_rd,
            uring_.get_sq_entries());
        poll_uring_(true, 0);
    }
}

// return the number of completions processed
// if blocking is true, will block until at least one completion is processed
size_t AsyncIO::poll_uring_(bool const blocking, unsigned const poll_rings_mask)
{
    // bit 0 in poll_rings_mask blocks read completions, bit 1 blocks write
    // completions
    MONAD_ASSERT((poll_rings_mask & 3) != 3);
    MONAD_ASSERT_PRINTF(
        !eager_completions_,
        "%s",
        "eager completions mode not yet implemented on Windows");
    auto const h = detail::AsyncIO_per_thread_state().enter_completions();
    MONAD_ASSERT(owning_tid_ == get_tl_tid());

    // Drain a synchronously-completed long read before touching the real
    // rings: it already finished inside submit_request_(iovec...) (Win32
    // IoRing has no scatter-read op), so there is nothing to wait for here.
    // Notifying it from this function -- never from submit_request_ itself
    // -- is what keeps read completions non-reentrant on Windows, matching
    // io_uring's guarantee that a read can never finish before the call
    // that submitted it returns.
    if (!sync_completed_long_reads_.empty() && (poll_rings_mask & 1) == 0) {
        auto const [state, bytes] = sync_completed_long_reads_.front();
        sync_completed_long_reads_.pop_front();
        MONAD_ASSERT(state != nullptr);
        --records_.inflight_rd;
        if (capture_io_latencies_) {
            state->elapsed = std::chrono::steady_clock::now() - state->initiated;
        }
        MONAD_WIN_HEAP_CHECK();
        // Long reads are heap-allocated with plain `new`
        // (initiate_async_read_update's read_long_update_sender branch), not
        // the connected_operation_storage_pool_ that read()/write() ops use,
        // so on completion they are freed with `delete`, never returned to
        // the pool -- mirrored from the is_read_or_write==false branch below.
        std::unique_ptr<erased_connected_operation> h3;
        if (state->lifetime_is_managed_internally()) {
            h3 = std::unique_ptr<erased_connected_operation>(state);
        }
        result<size_t> res = success(bytes);
        state->completed(std::move(res));
        MONAD_WIN_HEAP_CHECK();
        h3.reset();
        MONAD_WIN_HEAP_CHECK();
        return 1;
    }

    // SubmitIoRing(ring, 0, 0, ...) submits any queued SQEs without waiting,
    // the IoRing analogue of io_uring_peek_cqe(); SubmitIoRing(ring, 1,
    // INFINITE, ...) blocks until at least one completion is ready, the
    // analogue of io_uring_wait_cqe().
    auto try_pop = [](HIORING const ring, UINT32 const wait_operations,
                       UINT32 const milliseconds, IORING_CQE *const cqe) {
        UINT32 submitted = 0;
        HRESULT hr =
            SubmitIoRing(ring, wait_operations, milliseconds, &submitted);
        MONAD_ASSERT_PRINTF(
            SUCCEEDED(hr), "SubmitIoRing failed with HRESULT 0x%08lx", hr);
        hr = PopIoRingCompletion(ring, cqe);
        MONAD_ASSERT_PRINTF(
            SUCCEEDED(hr),
            "PopIoRingCompletion failed with HRESULT 0x%08lx",
            hr);
        return hr != S_FALSE;
    };

    auto &ts = detail::AsyncIO_per_thread_state();
    IORING_CQE cqe;
    bool got = false;

    if (wr_uring_ != nullptr && records_.inflight_wr > 0 &&
        (poll_rings_mask & 2) == 0) {
        got = try_pop(wr_uring_->get_handle(), 0, 0, &cqe);
        if (!got && (poll_rings_mask & 1) != 0 && blocking && ts.empty()) {
            got = try_pop(wr_uring_->get_handle(), 1, INFINITE, &cqe);
        }
        if (!got && (poll_rings_mask & 1) != 0) {
            return 0;
        }
    }

    if (!got) {
        bool const do_block =
            blocking && records_.inflight_wr == 0 && ts.empty();
        got = do_block ? try_pop(uring_.get_handle(), 1, INFINITE, &cqe)
                        : try_pop(uring_.get_handle(), 0, 0, &cqe);
        if (!got) {
            return 0;
        }
    }

    auto *const state =
        reinterpret_cast<erased_connected_operation *>(cqe.UserData);
    MONAD_ASSERT(state != nullptr);

    result<size_t> res(success(size_t{0}));
    if (SUCCEEDED(cqe.ResultCode)) {
        res = success(static_cast<size_t>(cqe.Information));
    }
    else if (cqe.ResultCode == HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)) {
        // ReadFile via IoRing reports reads at/past end-of-file as a failed
        // completion rather than a zero-byte success, unlike pread(2).
        res = success(size_t{0});
    }
    else {
        res = posix_code(hresult_to_errno_(cqe.ResultCode));
    }

    if (capture_io_latencies_) {
        state->elapsed = std::chrono::steady_clock::now() - state->initiated;
    }

    bool is_read_or_write = false;
    if (state->is_read()) {
        --records_.inflight_rd;
        is_read_or_write = true;
    }
    else if (state->is_write()) {
        --records_.inflight_wr;
        is_read_or_write = true;
    }
#ifndef NDEBUG
    else {
        MONAD_ABORT("unexpected completion operation type");
    }
#endif

    MONAD_WIN_HEAP_CHECK();
    erased_connected_operation_unique_ptr_type h2;
    std::unique_ptr<erased_connected_operation> h3;
    if (state->lifetime_is_managed_internally()) {
        if (is_read_or_write) {
            h2 = erased_connected_operation_unique_ptr_type{state};
        }
        else {
            h3 = std::unique_ptr<erased_connected_operation>(state);
        }
    }
    state->completed(std::move(res));
    MONAD_WIN_HEAP_CHECK();
    // Explicitly destruct h2/h3 before the final check so heap corruption
    // from the pool free or operator delete is caught here rather than
    // silently after function return.
    h2.reset();
    h3.reset();
    MONAD_WIN_HEAP_CHECK();
    return 1;
}

unsigned AsyncIO::deferred_initiations_in_flight() const noexcept
{
    auto const &ts = detail::AsyncIO_per_thread_state();
    return !ts.empty() && !ts.am_within_completions();
}

std::pair<unsigned, unsigned>
AsyncIO::io_uring_ring_entries_left(bool const for_wr_ring) const noexcept
{
    monad::io::Ring const *const ring = for_wr_ring ? wr_uring_ : &uring_;
    if (ring == nullptr) {
        return {0, 0};
    }
    unsigned const inflight =
        for_wr_ring ? records_.inflight_wr : records_.inflight_rd;
    unsigned const sqes = ring->get_sq_entries();
    unsigned const cqes = ring->get_cq_entries();
    return {
        (inflight < sqes) ? (sqes - inflight) : 0,
        (inflight < cqes) ? (cqes - inflight) : 0};
}

void AsyncIO::dump_fd_to(size_t const which, std::filesystem::path const &path)
{
    int const tofd = ::open(
        path.string().c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_BINARY, 0600);
    MONAD_ASSERT_PRINTF(
        tofd != -1, "open failed due to %s", std::strerror(errno));
    auto const untodfd = make_scope_exit([tofd]() noexcept { ::close(tofd); });

    auto const fromfd = seq_chunks_[which].chunk.read_fd();
    auto const total = seq_chunks_[which].chunk.size();

    // No fd+offset partial-copy primitive (copy_file_range equivalent) is
    // available on Windows; copy via a pread/pwrite loop instead.
    static constexpr size_t COPY_BUFFER_SIZE = 1UL << 20;
    std::vector<std::byte> buf(COPY_BUFFER_SIZE);
    file_offset_t off_in = fromfd.second;
    file_offset_t off_out = 0;
    file_offset_t remaining = total;
    while (remaining > 0) {
        size_t const to_read = (remaining < COPY_BUFFER_SIZE)
                                    ? static_cast<size_t>(remaining)
                                    : COPY_BUFFER_SIZE;
        ssize_t const nread = ::pread(
            fromfd.first,
            buf.data(),
            to_read,
            static_cast<off_t>(off_in));
        MONAD_ASSERT_PRINTF(
            nread >= 0, "pread failed due to %s", std::strerror(errno));
        if (nread == 0) {
            break;
        }
        ssize_t const nwritten = ::pwrite(
            tofd,
            buf.data(),
            static_cast<size_t>(nread),
            static_cast<off_t>(off_out));
        MONAD_ASSERT_PRINTF(
            nwritten == nread, "pwrite failed due to %s", std::strerror(errno));
        off_in += static_cast<file_offset_t>(nread);
        off_out += static_cast<file_offset_t>(nwritten);
        remaining -= static_cast<file_offset_t>(nread);
    }
}

unsigned char *AsyncIO::poll_uring_while_no_io_buffers_(bool const is_write)
{
    /* Prevent any new i/o initiation as we cannot exit until an i/o
    buffer becomes freed.
    */
    auto const h = detail::AsyncIO_per_thread_state().enter_completions();
    for (;;) {
        // If this assert fails, there genuinely
        // are not enough i/o buffers. This can happen if the caller
        // initiates more i/o than there are buffers available.
        if (0 == io_in_flight()) {
            std::cerr
                << "FATAL: no i/o buffers remaining. is_write = " << is_write
                << " within_completions_count = "
                << detail::AsyncIO_per_thread_state().within_completions_count
                << std::endl;
            MONAD_ABORT("no i/o buffers remaining");
        }
        // Reap completions until a buffer frees up, only reaping completions
        // for the write or other ring exclusively.
        poll_uring_(true, is_write ? 1 : 2);
        auto *mem = (is_write ? wr_pool_ : rd_pool_).alloc();
        if (mem != nullptr) {
            return mem;
        }
    }
}

MONAD_ASYNC_NAMESPACE_END

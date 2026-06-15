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

#ifndef _WIN32
    #include <bits/types/struct_iovec.h>
#endif
#include <category/core/io/buffers.hpp>

#include <category/core/assert.h>

#include <category/core/io/config.hpp>
#include <category/core/io/ring.hpp>

#ifndef _WIN32
    #include <liburing.h>
#endif

#include <bit>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <source_location>

MONAD_IO_NAMESPACE_BEGIN

Buffers::Buffers(
    Ring &ring, Ring *const wr_ring, size_t const read_count,
    size_t const write_count, size_t const read_size, size_t const write_size)
    : ring_{ring}
    , wr_ring_(wr_ring)
    , read_bits_{[=] {
        MONAD_ASSERT(std::has_single_bit(read_size));
        MONAD_ASSERT(read_size >= (1UL << 12));
        return static_cast<size_t>(std::countr_zero(read_size));
    }()}
    , write_bits_{[=]() -> size_t {
        if (write_count == 0 && write_size == 0) {
            return 0;
        }
        MONAD_ASSERT(std::has_single_bit(write_size));
        MONAD_ASSERT(write_size >= (1UL << 12));
        return static_cast<size_t>(std::countr_zero(write_size));
    }()}
    , read_buf_{read_count * read_size}
    , write_buf_{(write_count == 0 && write_size == 0) ? std::optional<HugeMem>() : std::optional<HugeMem>(write_count * write_size)}
    , read_count_{read_buf_.get_size() / read_size}
    , write_count_{
          (write_count == 0 && write_size == 0)
              ? 0
              : (write_buf_->get_size() / write_size)}
{
#ifdef _WIN32
    // Unlike Linux's io_uring_register_buffers (a direct syscall), IoRing's
    // BuildIoRingRegisterBuffers only enqueues an SQE; the registration
    // doesn't take effect (and the buffer indices it allocates aren't usable)
    // until that SQE is submitted and its completion observed.
    auto do_register_buffers = [](HIORING const ring,
                                   IORING_BUFFER_INFO const *const buffers,
                                   UINT32 const count,
                                   std::source_location loc =
                                       std::source_location::current()) {
        HRESULT hr = BuildIoRingRegisterBuffers(ring, count, buffers, 0);
        if (FAILED(hr)) {
            std::cerr << "FATAL: BuildIoRingRegisterBuffers in buffers.cpp "
                         "at line "
                      << loc.line() << " failed with HRESULT 0x" << std::hex
                      << hr << std::endl;
            std::terminate();
        }
        UINT32 submitted = 0;
        hr = SubmitIoRing(ring, 1, INFINITE, &submitted);
        if (FAILED(hr)) {
            std::cerr << "FATAL: SubmitIoRing in buffers.cpp at line "
                      << loc.line() << " failed with HRESULT 0x" << std::hex
                      << hr << std::endl;
            std::terminate();
        }
        IORING_CQE cqe;
        hr = PopIoRingCompletion(ring, &cqe);
        if (FAILED(hr) || FAILED(cqe.ResultCode)) {
            std::cerr
                << "FATAL: PopIoRingCompletion for RegisterBuffers in "
                   "buffers.cpp at line "
                << loc.line() << " failed with HRESULT 0x" << std::hex << hr
                << " / cqe.ResultCode 0x" << cqe.ResultCode << std::endl;
            std::terminate();
        }
    };
    // Registration order is preserved: read -> index 0, write -> index 1 on
    // the same ring (mixed); index 0 on each ring when segregated. This
    // matches Buffers::get_read_index()==0 / get_write_index()==1.
    if (wr_ring_ != nullptr) {
        IORING_BUFFER_INFO const read_info{
            read_buf_.get_data(), static_cast<UINT32>(read_buf_.get_size())};
        IORING_BUFFER_INFO const write_info{
            write_buf_.value().get_data(),
            static_cast<UINT32>(write_buf_.value().get_size())};
        do_register_buffers(ring_.get_handle(), &read_info, 1);
        do_register_buffers(wr_ring_->get_handle(), &write_info, 1);
    }
    else if (!write_buf_.has_value()) {
        IORING_BUFFER_INFO const read_info{
            read_buf_.get_data(), static_cast<UINT32>(read_buf_.get_size())};
        do_register_buffers(ring_.get_handle(), &read_info, 1);
    }
    else {
        IORING_BUFFER_INFO const infos[2]{
            {read_buf_.get_data(),
             static_cast<UINT32>(read_buf_.get_size())},
            {write_buf_.value().get_data(),
             static_cast<UINT32>(write_buf_.value().get_size())}};
        do_register_buffers(ring_.get_handle(), infos, 2);
    }
#else
    auto do_register_buffers = [](io_uring *const ring,
                                  const struct iovec *const iovecs,
                                  unsigned const nr_iovecs,
                                  std::source_location loc =
                                      std::source_location::current()) {
        if (int const errcode =
                io_uring_register_buffers(ring, iovecs, nr_iovecs);
            errcode < 0) {
            std::cerr
                << "FATAL: io_uring_register_buffers in buffer.cpp at line "
                << loc.line() << " failed with '" << strerror(-errcode)
                << "'. iovecs[0] = { " << iovecs[0].iov_base << ", "
                << iovecs[0].iov_len << " }" << std::endl;
            std::terminate();
        }
    };
    if (wr_ring_ != nullptr) {
        iovec const iov[2]{
            {.iov_base = read_buf_.get_data(), .iov_len = read_buf_.get_size()},
            {.iov_base = write_buf_.value().get_data(),
             .iov_len = write_buf_.value().get_size()}};
        do_register_buffers(&ring_.get_ring(), iov, 1);
        do_register_buffers(&wr_ring_->get_ring(), iov + 1, 1);
    }
    else if (!write_buf_.has_value()) {
        iovec const iov[2]{
            {.iov_base = read_buf_.get_data(),
             .iov_len = read_buf_.get_size()}};
        do_register_buffers(&ring_.get_ring(), iov, 1);
    }
    else {
        iovec const iov[2]{
            {.iov_base = read_buf_.get_data(), .iov_len = read_buf_.get_size()},
            {.iov_base = write_buf_.value().get_data(),
             .iov_len = write_buf_.value().get_size()}};
        do_register_buffers(&ring_.get_ring(), iov, 2);
    }
#endif
}

Buffers::~Buffers()
{
#ifdef _WIN32
    // IoRing has no unregister-buffers operation; CloseIoRing (in Ring's
    // destructor, which runs after Buffers') releases all registrations at
    // whole-ring granularity.
#else
    if (wr_ring_ != nullptr) {
        MONAD_ASSERT(!io_uring_unregister_buffers(&wr_ring_->get_ring()));
    }
    MONAD_ASSERT(!io_uring_unregister_buffers(&ring_.get_ring()));
#endif
}

MONAD_IO_NAMESPACE_END

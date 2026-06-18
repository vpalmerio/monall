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

#include "config.hpp"

#include <category/core/assert.h>

#include <concepts>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>

MONAD_ASYNC_NAMESPACE_BEGIN

//! The types suitable for rounding up or down
template <class T, unsigned bits>
concept safely_roundable_type =
    std::unsigned_integral<T> && (__CHAR_BIT__ * sizeof(T)) > bits;

template <unsigned bits, safely_roundable_type<bits> T>
inline constexpr T round_up_align(T const x) noexcept
{
    constexpr T mask = (T(1) << bits) - 1;
    return (x + mask) & ~mask;
}

template <unsigned bits>
inline constexpr chunk_offset_t round_up_align(chunk_offset_t x) noexcept
{
    constexpr file_offset_t mask = (file_offset_t(1) << bits) - 1;
    x.offset = (x.offset + mask) & ~mask;
    return x;
}

template <unsigned bits, safely_roundable_type<bits> T>
inline constexpr T round_down_align(T const x) noexcept
{
    constexpr T mask = ~((T(1) << bits) - 1);
    return x & mask;
}

template <unsigned bits>
inline constexpr chunk_offset_t round_down_align(chunk_offset_t x) noexcept
{
    constexpr file_offset_t mask = ~((file_offset_t(1) << bits) - 1);
    x.offset = x.offset & mask & chunk_offset_t::max_offset;
    return x;
}

//! Returns a temporary directory in which `O_DIRECT` files definitely work
extern std::filesystem::path const &working_temporary_directory();

//! Creates already deleted file so no need to clean it up
//! after
extern int make_temporary_inode() noexcept;

//! Resizes a file (already open for writing) to exactly `size_bytes`.
//! Returns 0 on success, -1 on failure (with errno set).
//!
//! On Linux this is a thin wrapper around ftruncate().
//!
//! On Windows, MinGW's ftruncate() routes through libmingwex.a → msvcrt's
//! _chsize_s(), which looks up the fd in msvcrt's CRT fd table.  Fds
//! opened via _open_osfhandle (ucrtbase) live in ucrtbase's *separate*
//! table and are invisible to msvcrt → EBADF.  Using _get_osfhandle()
//! (ucrtbase) to retrieve the underlying Win32 HANDLE and then calling
//! SetFilePointerEx + SetEndOfFile bypasses both the fd-table mismatch and
//! the 32-bit off_t overflow that would silently truncate to 0 on LLP64.
inline int resize_file(int const fd, int64_t const size_bytes)
{
#ifdef _WIN32
    // _get_osfhandle is from ucrtbase, so it finds the fd that
    // _open_osfhandle placed in ucrtbase's table.
    HANDLE const h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    LARGE_INTEGER li;
    li.QuadPart = size_bytes;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        errno = EIO;
        return -1;
    }
    if (!SetEndOfFile(h)) {
        errno = EIO;
        return -1;
    }
    return 0;
#else
    return ::ftruncate(fd, static_cast<off_t>(size_bytes));
#endif
}

//! Returns a path that can be used to reopen the file behind `fd` by path,
//! including for an unlinked/anonymous inode (e.g. one obtained from
//! make_temporary_inode()).
//!
//! On Linux this is the /proc/self/fd/<fd> procfs alias.
//!
//! Windows has no equivalent magic path, so this recovers the real
//! underlying path via GetFinalPathNameByHandleA, which works even for a
//! FILE_FLAG_DELETE_ON_CLOSE handle (mirrors
//! storage_pool::device_t::current_path() in storage_pool_windows.cpp).
inline std::filesystem::path path_for_fd(int const fd)
{
#ifdef _WIN32
    HANDLE const h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    MONAD_ASSERT_PRINTF(
        h != INVALID_HANDLE_VALUE,
        "_get_osfhandle failed due to %s",
        strerror(errno));
    char buf[MAX_PATH];
    DWORD const len =
        GetFinalPathNameByHandleA(h, buf, sizeof(buf), FILE_NAME_NORMALIZED);
    MONAD_ASSERT_PRINTF(
        len != 0 && len < sizeof(buf),
        "GetFinalPathNameByHandleA failed due to %lu",
        GetLastError());
    return std::filesystem::path(std::string(buf, len));
#else
    return "/proc/self/fd/" + std::to_string(fd);
#endif
}

//! Creates a temporary file in the directory of `template_path` and returns
//! the open fd (guaranteed in ucrtbase's CRT fd table on Windows) plus the
//! actual file path.
//!
//! On Linux this wraps mkstemp() with a path::string() round-trip so that
//! std::filesystem::path::native() (wchar_t on Windows) never reaches
//! mkstemp()'s narrow-char API.
//!
//! On Windows mkstemp() routes through libmingwex.a → _sopen() in
//! msvcrt.dll, placing the fd in msvcrt's CRT fd table.  All I/O helpers
//! used downstream (fstat, ftruncate, pread, pwrite, _get_osfhandle) come
//! from ucrtbase.dll and use its *separate* fd table, so the msvcrt fd is
//! invisible to them (→ EBADF).  The Windows path uses GetTempFileNameA +
//! CreateFileA + _open_osfhandle (ucrtbase) to put the fd in the right
//! table, matching the pattern in compat.h's memfd_create.
inline std::pair<int, std::filesystem::path>
make_temp_file(std::filesystem::path const &template_path)
{
#ifdef _WIN32
    std::string const dir = template_path.parent_path().string();
    char out_path[MAX_PATH];
    // GetTempFileNameA creates the file and writes its final path into
    // out_path; only the first 3 chars of the prefix string are used.
    if (GetTempFileNameA(dir.c_str(), "mnd", 0, out_path) == 0) {
        errno = EIO;
        return {-1, {}};
    }
    // Open the already-created file for R/W.  No FILE_FLAG_DELETE_ON_CLOSE:
    // the caller closes this fd after ftruncate, then reopens the file by
    // path via storage_pool; the test fixture destructor deletes it.
    // FILE_SHARE_WRITE is required because tests may hold this fd open while
    // storage_pool then opens the same path with GENERIC_WRITE; Windows
    // rejects that second open if the first handle didn't set FILE_SHARE_WRITE
    // (POSIX open() has no such restriction, so we match that semantics here).
    HANDLE const h = CreateFileA(
        out_path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DeleteFileA(out_path);
        errno = EIO;
        return {-1, {}};
    }
    int const fd =
        _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY | _O_NOINHERIT);
    if (fd == -1) {
        CloseHandle(h);
        DeleteFileA(out_path);
        return {-1, {}};
    }
    return {fd, std::filesystem::path(out_path)};
#else
    std::string buf = template_path.string();
    int const fd = ::mkstemp(buf.data());
    return {fd, std::filesystem::path(buf)};
#endif
}

MONAD_ASYNC_NAMESPACE_END

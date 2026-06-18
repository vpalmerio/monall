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

#include <category/async/util.hpp>

#include <category/core/assert.h>
#include <category/core/compat.h>

#include <cerrno>
#include <cstring>
#include <string>

MONAD_ASYNC_NAMESPACE_BEGIN

std::filesystem::path const &working_temporary_directory()
{
    // Linux's version exists mainly to AVOID tmpfs (O_DIRECT doesn't work
    // there), via fstatfs()/TMPFS_MAGIC rejection. NTFS temporary
    // directories are real disk-backed and Windows has no O_DIRECT/tmpfs
    // distinction, so that detection logic has no analog here -- just use
    // whatever GetTempPathA resolves (TMP/TEMP/USERPROFILE/Windows dir, in
    // that order).
    static std::filesystem::path const v = [] {
        char buf[MAX_PATH + 1];
        DWORD const n = GetTempPathA(sizeof(buf), buf);
        MONAD_ASSERT_PRINTF(
            n != 0 && n < sizeof(buf),
            "GetTempPathA failed due to %lu",
            GetLastError());
        return std::filesystem::path(std::string(buf, n));
    }();
    return v;
}

int make_temporary_inode() noexcept
{
    // memfd_create() in category/core/compat.h already implements an
    // auto-delete-on-close temporary file (GetTempFileNameA +
    // FILE_FLAG_DELETE_ON_CLOSE + _open_osfhandle), which is exactly the
    // "already deleted, no cleanup needed" file this function promises.
    // Reuse it rather than duplicating the CreateFileA/_open_osfhandle dance.
    // MONAD_MEMFD_OVERLAPPED requests an overlapped handle: this fd backs
    // storage_pool's anonymous-inode device, which AsyncIO issues IoRing
    // reads/writes against just like a named-file device.
    int const fd = ::memfd_create("monad", MONAD_MEMFD_OVERLAPPED);
    MONAD_ASSERT_PRINTF(fd != -1, "failed due to %s", strerror(errno));
    return fd;
}

MONAD_ASYNC_NAMESPACE_END

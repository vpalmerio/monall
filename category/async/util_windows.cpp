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

// Windows placeholder for category/async/util.cpp. The helpers here rely on
// Linux-specific O_TMPFILE/O_DIRECT temporary file semantics and statfs(2),
// which have no equivalent on Windows. No Phase 2 test calls these -- they
// abort if ever reached. See the Windows port plan for the follow-up
// storage_pool functional port.

#include <category/async/util.hpp>

#include <category/core/assert.h>

MONAD_ASYNC_NAMESPACE_BEGIN

std::filesystem::path const &working_temporary_directory()
{
    MONAD_ABORT_PRINTF(
        "working_temporary_directory not yet implemented on Windows");
}

int make_temporary_inode() noexcept
{
    MONAD_ABORT_PRINTF("make_temporary_inode not yet implemented on Windows");
}

MONAD_ASYNC_NAMESPACE_END

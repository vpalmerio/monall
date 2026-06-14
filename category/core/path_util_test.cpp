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

#include <iostream>

#include <gtest/gtest.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

#ifdef _WIN32
    #include <category/core/compat.h> // for AT_FDCWD
#endif

#include <category/core/path_util.h>

TEST(PathUtil, Basic)
{
    int rc;
    int dirfd;
    char pathbuf[PATH_MAX];
    constexpr char const TEST_DIR[] = "/tmp/monad-path-util-test/xyz";
    constexpr char const PARENT_TEST_DIR[] = "/tmp/monad-path-util-test";

    rc = monad_path_open_subdir(
        AT_FDCWD,
        TEST_DIR,
        MONAD_PATH_NO_CREATE,
        &dirfd,
        pathbuf,
        sizeof pathbuf);
    ASSERT_EQ(rc, ENOENT); // Can't create the suffix, and it doesn't exist
    ASSERT_EQ(dirfd, -1);

    // Try again, this time we can create it
    rc = monad_path_open_subdir(
        AT_FDCWD, TEST_DIR, S_IRWXU, &dirfd, pathbuf, sizeof pathbuf);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(dirfd, -1);
    // Use iostream rather than cstdio's fprintf here: on this mingw build,
    // fprintf(stderr, ...) crashes inside _lock_file when this binary is
    // linked against the system msvcrt.dll's _lock (a stale CRT pulled in by
    // some dependency), whose tiny static _iob[] table doesn't recognize
    // ucrt's stderr FILE* and indexes out of bounds.
    std::cerr << "full path is: " << pathbuf << "\n";
    (void)close(dirfd);

    // Try again; we can't create it, but that's OK: it's there now
    rc = monad_path_open_subdir(
        AT_FDCWD,
        TEST_DIR,
        MONAD_PATH_NO_CREATE,
        &dirfd,
        pathbuf,
        sizeof pathbuf);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(dirfd, -1);
    (void)close(dirfd);

    // Remove the test directory
    ASSERT_EQ(rmdir(TEST_DIR), 0);
    ASSERT_EQ(rmdir(PARENT_TEST_DIR), 0);

    // Either parameter can be nullptr
    rc = monad_path_open_subdir(
        AT_FDCWD,
        TEST_DIR,
        MONAD_PATH_NO_CREATE,
        nullptr,
        nullptr,
        sizeof pathbuf);
    ASSERT_EQ(rc, ENOENT);
}

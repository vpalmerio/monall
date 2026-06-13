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

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <category/core/cleanup.h> // NOLINT(misc-include-cleaner)
#include <category/core/compat.h>
#include <category/core/path_util.h>

// Silence clang-tidy complaining about trying to close AT_FDCWD
static void tidy_close(int const fd)
{
    if (fd >= 0) {
        (void)close(fd);
    }
}

int monad_path_append(
    char **const dst, char const *const src, size_t *const size)
{
    if (dst == nullptr || src == nullptr || size == nullptr) {
        return EFAULT;
    }
    if (*dst == nullptr) {
        return ENOBUFS;
    }
    if (**dst != '\0') {
        return EINVAL;
    }
    if (*size == 0) {
        return ERANGE;
    }
    **dst = '/';
    *dst += 1;
    *size -= 1;
    size_t const n = strlcpy(*dst, src, *size);
    if (n >= *size) {
        *dst += *size;
        *size = 0;
        return ERANGE;
    }
    *dst += n;
    *size -= n;
    return 0;
}

#ifdef _WIN32

// Windows has no dirfd-relative `*at` family of system calls, so instead of
// walking the path one directory-fd-relative component at a time, we build
// up an absolute path string (`curpath`) alongside `pathbuf` and use it with
// ordinary path-based Win32/CRT calls; each component is still validated and
// (optionally) created one at a time, preserving the property that `pathbuf`
// only ever contains path components that were successfully translated.
int monad_path_open_subdir(
    int const init_dirfd, char const *const path_suffix, mode_t const mode,
    int *const final_dirfd, char *pathbuf, size_t pathbuf_size)
{
    char *dir_name;
    char *tokctx;
    int curfd = -1;
    int rc = 0;
    bool const can_create_dirs = (mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != 0;
    char curpath[4096];
    size_t curpath_len;

    if (pathbuf != nullptr) {
        *pathbuf = '\0';
    }
    if (final_dirfd != nullptr) {
        *final_dirfd = -1;
    }
    if (path_suffix == nullptr) {
        if (final_dirfd != nullptr) {
            *final_dirfd = init_dirfd;
        }
        return 0;
    }

    if (init_dirfd == AT_FDCWD) {
        curpath_len = 0;
        if (*path_suffix == '/') {
            curpath[0] = '/';
            curpath_len = 1;
        }
        curpath[curpath_len] = '\0';
    }
    else {
        HANDLE const h = (HANDLE)_get_osfhandle(init_dirfd);
        if (h == INVALID_HANDLE_VALUE) {
            return EBADF;
        }
        DWORD const n = GetFinalPathNameByHandleA(
            h, curpath, sizeof curpath, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n == 0 || n >= sizeof curpath) {
            return EIO;
        }
        curpath_len = n;
        // Strip the `\\?\` extended-length prefix so that the path remains
        // palatable to the CRT's _mkdir()
        if (curpath_len >= 4 && memcmp(curpath, "\\\\?\\", 4) == 0) {
            memmove(curpath, curpath + 4, curpath_len - 4 + 1);
            curpath_len -= 4;
        }
    }

    // NOLINTBEGIN(clang-analyzer-unix.Malloc)
    char *const path_components [[gnu::cleanup(cleanup_free)]] =
        strdup(path_suffix);
    if (path_components == nullptr) {
        return errno;
    }
    for (dir_name = strtok_r(path_components, "/", &tokctx); dir_name;
         dir_name = strtok_r(nullptr, "/", &tokctx)) {
        size_t const name_len = strlen(dir_name);

        if (pathbuf != nullptr) {
            rc = monad_path_append(&pathbuf, dir_name, &pathbuf_size);
            if (rc != 0) {
                goto Done;
            }
        }
        if (curpath_len + 1 + name_len >= sizeof curpath) {
            rc = ENAMETOOLONG;
            goto Done;
        }
        curpath[curpath_len++] = '/';
        memcpy(curpath + curpath_len, dir_name, name_len);
        curpath_len += name_len;
        curpath[curpath_len] = '\0';

        if (can_create_dirs && _mkdir(curpath) == -1 && errno != EEXIST) {
            rc = errno;
            goto Done;
        }
        HANDLE const dirh = CreateFileA(
            curpath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (dirh == INVALID_HANDLE_VALUE) {
            DWORD const err = GetLastError();
            rc = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                     ? ENOENT
                 : (err == ERROR_ACCESS_DENIED) ? EACCES
                                                 : EIO;
            goto Done;
        }
        int const nextfd = _open_osfhandle((intptr_t)dirh, _O_RDONLY);
        if (nextfd == -1) {
            rc = errno;
            CloseHandle(dirh);
            goto Done;
        }
        tidy_close(curfd);
        curfd = nextfd;
    }

Done:
    if (final_dirfd != nullptr && rc == 0) {
        *final_dirfd = curfd;
    }
    else {
        tidy_close(curfd);
    }
    return rc;
    // NOLINTEND(clang-analyzer-unix.Malloc)
}

#else

int monad_path_open_subdir(
    int const init_dirfd, char const *const path_suffix, mode_t const mode,
    int *const final_dirfd, char *pathbuf, size_t pathbuf_size)
{
    char *dir_name;
    char *tokctx;
    int curfd;
    int rc = 0;
    bool const can_create_dirs = (mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != 0;
#ifdef O_PATH
    constexpr int OPEN_FLAGS = O_DIRECTORY | O_PATH;
#else
    constexpr int OPEN_FLAGS = O_DIRECTORY;
#endif

    if (pathbuf != nullptr) {
        *pathbuf = '\0';
    }
    if (final_dirfd != nullptr) {
        // Ensure the caller doesn't accidentally close something (e.g., stdin)
        // if they don't initialize *final_dirfd, then unconditionally close
        // upon failure
        *final_dirfd = -1;
    }

    if (path_suffix == nullptr) {
        if (final_dirfd != nullptr) {
            *final_dirfd = init_dirfd;
        }
        return 0;
    }

    // Setup curfd
    if (init_dirfd == AT_FDCWD) {
        curfd = AT_FDCWD;
        if (*path_suffix == '/') {
            // Properly support absolute paths with AT_FDCWD; we wouldn't get
            // it right without this special case because we walk relative
            // paths one path component at a time, so e.g., "/tmp/xyz" would
            // accidentally mkdirat <cwd>/tmp on the first iteration
            curfd = open("/", OPEN_FLAGS);
        }
    }
    else {
        // This allows us to unconditionally close the curfd on any path,
        // simplifying the logic
        curfd = dup(init_dirfd);
    }
    if (curfd == -1) {
        return errno; // dup(2) or open(2) error
    }

    // NOLINTBEGIN(clang-analyzer-unix.Malloc)
    char *const path_components [[gnu::cleanup(cleanup_free)]] =
        strdup(path_suffix);
    if (path_components == nullptr) {
        rc = errno;
        goto Done;
    }
    for (dir_name = strtok_r(path_components, "/", &tokctx); dir_name;
         dir_name = strtok_r(nullptr, "/", &tokctx)) {
        // This loop iterates over the path components in a path string; each
        // path component is expected to be the name of a directory.
        //
        // Within this loop, `dir_name` refers to the next path component and
        // `curfd` is an open file descriptor to the parent directory of
        // `dir_name`; the "walk" involves:
        //
        //   - appending the `dir_name` to `pathbuf`; we do this first so that
        //     the user can tell which path segment an errno(3) code applies to
        //     in case one of the next steps fails
        //
        //   - creating a directory named `dir_name` if it doesn't exist and
        //     we're allowed to create directories
        //
        //   - opening a file descriptor to `dir_name` as the new `curfd` with
        //     O_DIRECTORY (thereby checking if it is a directory in case we
        //     got EEXIST from mkdirat(2) but it is some other type of file)
        //
        // When we're done, `curfd` is an open file descriptor to the last
        // directory in the path
        int nextfd;
        int prevfd;
        if (pathbuf != nullptr) {
            rc = monad_path_append(&pathbuf, dir_name, &pathbuf_size);
            if (rc != 0) {
                goto Done;
            }
        }
        if (can_create_dirs && mkdirat(curfd, dir_name, mode) == -1 &&
            errno != EEXIST) {
            rc = errno;
            goto Done;
        }
        nextfd = openat(curfd, dir_name, OPEN_FLAGS);
        if (nextfd == -1) {
            rc = errno;
            goto Done;
        }
        prevfd = curfd;
        curfd = nextfd;
        tidy_close(prevfd);
    }

Done:
    if (final_dirfd != nullptr && rc == 0) {
        *final_dirfd = curfd;
    }
    else {
        tidy_close(curfd);
    }
    return rc;
    // NOLINTEND(clang-analyzer-unix.Malloc)
}

#endif // _WIN32

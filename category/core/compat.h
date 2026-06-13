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

/// @file
///
/// Small portability shims for POSIX interfaces that mingw-w64 does not
/// provide, but which are used in a handful of places in category/core.
/// On non-Windows platforms this header has no effect.

#ifdef _WIN32

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <direct.h>
    #include <fcntl.h>
    #include <io.h>
    #include <stdarg.h>
    #include <stddef.h>
    #include <stdint.h>
    #include <stdio.h>
    #include <string.h>
    #include <sys/types.h>
    #include <unistd.h>

    #ifndef AT_FDCWD
        #define AT_FDCWD (-100)
    #endif

    // sys/mman.h
    #define PROT_NONE  0x0
    #define PROT_READ  0x1
    #define PROT_WRITE 0x2

    #define MAP_SHARED    0x01
    #define MAP_PRIVATE   0x02
    #define MAP_FIXED     0x10
    #define MAP_ANONYMOUS 0x20
    #define MAP_ANON      MAP_ANONYMOUS
    #define MAP_POPULATE  0
    #define MAP_NORESERVE 0
    #define MAP_HUGETLB   0
    #define MAP_HUGE_2MB  0

    #define MAP_FAILED ((void *)-1)

    // sys/file.h
    #define LOCK_SH 1
    #define LOCK_EX 2
    #define LOCK_NB 4
    #define LOCK_UN 8

    #ifdef __cplusplus
extern "C"
    {
    #endif

static inline size_t
strlcpy(char *const dst, char const *const src, size_t const dstsize)
{
    size_t const srclen = strlen(src);
    if (dstsize != 0) {
        size_t const copylen = srclen < dstsize ? srclen : dstsize - 1;
        memcpy(dst, src, copylen);
        dst[copylen] = '\0';
    }
    return srclen;
}

__attribute__((format(gnu_printf, 2, 3))) static inline int
dprintf(int const fd, char const *const fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int const n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t const len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
        (void)write(fd, buf, (unsigned)len);
    }
    return n;
}

// Shared table used by mmap()/munmap() below to recover the Win32 file
// mapping object handle that corresponds to a view's base address; the table
// is a function-local static, so each translation unit that calls mmap() and
// munmap() shares its own private table (mmap/munmap pairs never cross a
// translation unit in this codebase)
static inline HANDLE
monad_compat_mmap_table(void *const addr, HANDLE const mapping, bool const insert)
{
    static struct
    {
        void *addr;
        HANDLE mapping;
    } table[32];
    static volatile long lock;

    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {
    }
    HANDLE result = nullptr;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        bool const slot_matches =
            insert ? table[i].addr == nullptr : table[i].addr == addr;
        if (slot_matches) {
            if (insert) {
                table[i].addr = addr;
                table[i].mapping = mapping;
                result = mapping;
            }
            else {
                result = table[i].mapping;
                table[i].addr = nullptr;
                table[i].mapping = nullptr;
            }
            break;
        }
    }
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
    return result;
}

// Minimal mmap()/munmap() shim covering the simple file-backed MAP_SHARED
// mappings used by category/core/event; MAP_ANONYMOUS (used only for the
// event ring payload buffer "wraparound" trick) is handled separately with
// the Win32 placeholder-VA APIs and is not supported here
static inline void *mmap(
    void *const addr, size_t const length, int const prot, int const flags,
    int const fd, off_t const offset)
{
    (void)addr;
    if (flags & MAP_ANONYMOUS) {
        errno = ENOTSUP;
        return MAP_FAILED;
    }
    DWORD const protect = (prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
    DWORD const access =
        (prot & PROT_WRITE) ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ;
    HANDLE const file = (HANDLE)_get_osfhandle(fd);
    if (file == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return MAP_FAILED;
    }
    HANDLE const mapping =
        CreateFileMappingA(file, nullptr, protect, 0, 0, nullptr);
    if (mapping == nullptr) {
        errno = EACCES;
        return MAP_FAILED;
    }
    void *const base = MapViewOfFile(
        mapping,
        access,
        (DWORD)((uint64_t)offset >> 32),
        (DWORD)((uint64_t)offset & 0xffffffffu),
        length);
    if (base == nullptr) {
        CloseHandle(mapping);
        errno = EACCES;
        return MAP_FAILED;
    }
    if (monad_compat_mmap_table(base, mapping, true) == nullptr) {
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return base;
}

static inline int munmap(void *const addr, size_t const length)
{
    (void)length;
    HANDLE const mapping = monad_compat_mmap_table(addr, nullptr, false);
    UnmapViewOfFile(addr);
    if (mapping != nullptr) {
        CloseHandle(mapping);
    }
    return 0;
}

// Create an unnamed, auto-deleting temporary file and return a CRT file
// descriptor for it; used as the destination for zstd-decompressed event
// ring snapshot data
static inline int
memfd_create(char const *const name, unsigned int const flags)
{
    (void)flags;
    char tmp_path[MAX_PATH];
    char tmp_file[MAX_PATH];
    if (GetTempPathA(sizeof tmp_path, tmp_path) == 0) {
        errno = EIO;
        return -1;
    }
    if (GetTempFileNameA(tmp_path, name, 0, tmp_file) == 0) {
        errno = EIO;
        return -1;
    }
    HANDLE const h = CreateFileA(
        tmp_file,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EIO;
        return -1;
    }
    int const fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
    if (fd == -1) {
        CloseHandle(h);
    }
    return fd;
}

// Extend the file so that it can hold [offset, offset+len); unlike real
// posix_fallocate(2) this does not guarantee the space is physically
// reserved, but NTFS will report ENOSPC up front for sufficiently large
// requests, which is the property the callers in this codebase rely on
static inline int
posix_fallocate(int const fd, off_t const offset, off_t const len)
{
    HANDLE const h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        return EBADF;
    }
    LARGE_INTEGER cur;
    if (!GetFileSizeEx(h, &cur)) {
        return EIO;
    }
    LARGE_INTEGER target;
    target.QuadPart = (LONGLONG)offset + (LONGLONG)len;
    if (target.QuadPart <= cur.QuadPart) {
        return 0;
    }
    if (!SetFilePointerEx(h, target, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(h)) {
        return EIO;
    }
    (void)SetFilePointerEx(h, cur, nullptr, FILE_BEGIN);
    return 0;
}

    #ifdef __cplusplus
    }
    #endif

#endif // _WIN32

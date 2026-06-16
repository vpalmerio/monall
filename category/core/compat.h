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

    // stdio.h: renameat2() flag; unused by the renameat2() shim below since
    // Windows rename() already implements RENAME_NOREPLACE semantics
    #define RENAME_NOREPLACE 1

    // fcntl.h: close-on-exec equivalent
    #ifndef O_CLOEXEC
        #define O_CLOEXEC O_NOINHERIT
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

    #define MS_SYNC 4

    // sys/mman.h: madvise() advice values
    #define MADV_NORMAL     0
    #define MADV_RANDOM     1
    #define MADV_SEQUENTIAL 2
    #define MADV_WILLNEED   3
    #define MADV_DONTNEED   4

    // sys/file.h
    #define LOCK_SH 1
    #define LOCK_EX 2
    #define LOCK_NB 4
    #define LOCK_UN 8

    // sys/uio.h
    struct iovec
    {
        void *iov_base;
        size_t iov_len;
    };

    #ifdef __cplusplus
extern "C"
    {
    #endif

// Copies src (including the terminating '\0') to dst and returns a pointer
// to the terminating '\0' written to dst, like glibc's stpcpy()
static inline char *stpcpy(char *const dst, char const *const src)
{
    size_t const len = strlen(src);
    memcpy(dst, src, len + 1);
    return dst + len;
}

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
// mapping object handle and the view's real (allocation-granularity-aligned)
// base address from the address returned to the caller; the table is a
// function-local static, so each translation unit that calls mmap() and
// munmap() shares its own private table (mmap/munmap pairs never cross a
// translation unit in this codebase)
static inline HANDLE monad_compat_mmap_table(
    void *const addr, HANDLE const mapping, void *const real_base,
    void **const real_base_out, bool const insert)
{
    static struct
    {
        void *addr;
        HANDLE mapping;
        void *real_base;
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
                table[i].real_base = real_base;
                result = mapping;
            }
            else {
                result = table[i].mapping;
                if (real_base_out != nullptr) {
                    *real_base_out = table[i].real_base;
                }
                table[i].addr = nullptr;
                table[i].mapping = nullptr;
                table[i].real_base = nullptr;
            }
            break;
        }
    }
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
    return result;
}

// VirtualAlloc2/MapViewOfFile3/UnmapViewOfFile2 implement the "placeholder
// VA" APIs (Windows 10 1803+) used by the MAP_ANONYMOUS|PROT_NONE and
// MAP_FIXED cases in mmap()/munmap() below for category/mpt's
// DbMetadataContext ring-storage layout. mingw-w64's headers declare these
// only when NTDDI_VERSION >= NTDDI_WIN10_RS4, and even then its import
// libraries do not export them, so resolve them dynamically from
// kernelbase.dll at first use -- same approach and rationale as
// category/core/event/event_ring.c's resolve_placeholder_va_apis().
typedef PVOID(WINAPI *MonadVirtualAlloc2Fn)(
    HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID(WINAPI *MonadMapViewOfFile3Fn)(
    HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
    MEM_EXTENDED_PARAMETER *, ULONG);
typedef WINBOOL(WINAPI *MonadUnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);

struct MonadPlaceholderVaApis
{
    MonadVirtualAlloc2Fn VirtualAlloc2;
    MonadMapViewOfFile3Fn MapViewOfFile3;
    MonadUnmapViewOfFile2Fn UnmapViewOfFile2;
};

static inline struct MonadPlaceholderVaApis const *
monad_compat_placeholder_va_apis(void)
{
    static struct MonadPlaceholderVaApis apis;
    static volatile long resolved;
    if (!__atomic_load_n(&resolved, __ATOMIC_ACQUIRE)) {
        HMODULE const kernelbase = GetModuleHandleA("kernelbase.dll");
        if (kernelbase != nullptr) {
            apis.VirtualAlloc2 = (MonadVirtualAlloc2Fn)(void *)GetProcAddress(
                kernelbase, "VirtualAlloc2");
            apis.MapViewOfFile3 =
                (MonadMapViewOfFile3Fn)(void *)GetProcAddress(
                    kernelbase, "MapViewOfFile3");
            apis.UnmapViewOfFile2 =
                (MonadUnmapViewOfFile2Fn)(void *)GetProcAddress(
                    kernelbase, "UnmapViewOfFile2");
        }
        __atomic_store_n(&resolved, 1, __ATOMIC_RELEASE);
    }
    return &apis;
}

// Bookkeeping table for category/mpt's placeholder-VA chunk-slot mappings
// (see the MAP_ANONYMOUS|PROT_NONE and MAP_FIXED cases in mmap() below):
// maps the address of a currently-installed MAP_FIXED view to the Win32
// file-mapping HANDLE backing it, so a later idempotent re-install
// (DbMetadataContext::install_chunk_mapping_) or whole-reservation
// munmap() can CloseHandle it after UnmapViewOfFile2 reverts the view back
// to a placeholder. Same per-TU sharing rationale as
// monad_compat_mmap_table above -- the installer and the destructor that
// releases the reservation are both defined in db_metadata_context.cpp.
static inline HANDLE monad_compat_placeholder_table(
    void *const addr, HANDLE const mapping, bool const insert)
{
    static struct
    {
        void *addr;
        HANDLE mapping;
    } table[64];
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
// mappings used by category/core/event, plus the placeholder-VA reserve /
// MAP_FIXED-install pattern category/mpt's DbMetadataContext uses to lay out
// its ring-storage metadata mappings (MAP_ANONYMOUS is used only for the
// event ring payload buffer "wraparound" trick, handled separately with the
// Win32 placeholder-VA APIs, and for DbMetadataContext's PROT_NONE
// reservations, handled here).
static inline void *mmap(
    void *const addr, size_t const length, int const prot, int const flags,
    int const fd, off_t const offset)
{
    // DbMetadataContext reserves a PROT_NONE anonymous VA range up front
    // (db_metadata_context.hpp:map_ring_storage_) and later MAP_FIXED-maps
    // individual cnv-chunk file regions into slots within it. MapViewOfFile
    // cannot place a view at a caller-chosen address, so the reservation is
    // made with VirtualAlloc2's MEM_RESERVE_PLACEHOLDER instead; the
    // MAP_FIXED case below splits/replaces pieces of it with
    // MapViewOfFile3(MEM_REPLACE_PLACEHOLDER).
    if ((flags & MAP_ANONYMOUS) && addr == nullptr && prot == PROT_NONE) {
        struct MonadPlaceholderVaApis const *const apis =
            monad_compat_placeholder_va_apis();
        if (apis->VirtualAlloc2 == nullptr) {
            errno = ENOTSUP;
            return MAP_FAILED;
        }
        void *const r = apis->VirtualAlloc2(
            nullptr,
            nullptr,
            length,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
            PAGE_NOACCESS,
            nullptr,
            0);
        if (r == nullptr) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
        return r;
    }
    if ((flags & MAP_FIXED) && addr != nullptr) {
        struct MonadPlaceholderVaApis const *const apis =
            monad_compat_placeholder_va_apis();
        if (apis->MapViewOfFile3 == nullptr || apis->UnmapViewOfFile2 == nullptr) {
            errno = ENOTSUP;
            return MAP_FAILED;
        }
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
        if (mbi.Type == MEM_MAPPED) {
            // Idempotent re-install (grow/shrink/activate/replay): revert
            // the previously-installed view back to a same-size placeholder
            // -- MapViewOfFile3's MEM_REPLACE_PLACEHOLDER requires the
            // target to be a placeholder of the exact view size -- and
            // close its now-stale mapping handle.
            if (!apis->UnmapViewOfFile2(
                    GetCurrentProcess(), addr, MEM_PRESERVE_PLACEHOLDER)) {
                errno = EINVAL;
                return MAP_FAILED;
            }
            HANDLE const old_mapping =
                monad_compat_placeholder_table(addr, nullptr, false);
            if (old_mapping != nullptr) {
                CloseHandle(old_mapping);
            }
        }
        else if (length < mbi.RegionSize || mbi.AllocationBase != addr) {
            // Two cases both require a VirtualFree split before mapping:
            //
            // 1. length < mbi.RegionSize: first-time slot mapping, not the
            //    last slot in the reservation -- split an exact
            //    `length`-sized placeholder out of the containing
            //    reservation placeholder.
            //
            // 2. mbi.AllocationBase != addr: the slot's placeholder is
            //    already the right size (RegionSize == length) but is NOT
            //    standalone -- it was created by a prior mid-placeholder
            //    split (e.g. when a NULL_CHUNK slot was skipped in
            //    map_ring_storage_, leaving the placeholder for that slot
            //    and the next slot merged). MapViewOfFile3
            //    (MEM_REPLACE_PLACEHOLDER) requires AllocationBase ==
            //    addr; calling VirtualFree here converts this non-standalone
            //    piece into a proper standalone placeholder that satisfies
            //    that invariant.
            if (!VirtualFree(
                    addr, length, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                errno = ENOMEM;
                return MAP_FAILED;
            }
        }
        // else: AllocationBase == addr and RegionSize == length, so this
        // placeholder is already standalone and exactly the right size.
        // VirtualFree(MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER) would fail with
        // ERROR_INVALID_ADDRESS (487) here, so skip it.
        // MapViewOfFile3(MEM_REPLACE_PLACEHOLDER) below can target it directly.
        DWORD const protect =
            (prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
        HANDLE const file = (HANDLE)_get_osfhandle(fd);
        if (file == INVALID_HANDLE_VALUE) {
            // Use OutputDebugStringA + snprintf rather than fprintf(stderr)
            // to avoid a crash when the caller's CRT (ucrt64) differs from
            // a loaded DLL's CRT (msvcrt) -- fprintf tries to lock the other
            // CRT's stdout/stderr FILE*, which corrupts its lock state.
            char _dbg_buf[128];
            snprintf(_dbg_buf, sizeof(_dbg_buf),
                     "mmap MAP_FIXED: _get_osfhandle(fd=%d) failed: errno=%d\n",
                     fd, errno);
            OutputDebugStringA(_dbg_buf);
            errno = EBADF;
            return MAP_FAILED;
        }
        HANDLE const mapping =
            CreateFileMappingA(file, nullptr, protect, 0, 0, nullptr);
        if (mapping == nullptr) {
            char _dbg_buf[128];
            snprintf(_dbg_buf, sizeof(_dbg_buf),
                     "mmap MAP_FIXED: CreateFileMappingA failed: "
                     "GetLastError=%lu protect=%lu\n",
                     (unsigned long)GetLastError(), (unsigned long)protect);
            OutputDebugStringA(_dbg_buf);
            errno = EACCES;
            return MAP_FAILED;
        }
        void *const view = apis->MapViewOfFile3(
            mapping,
            GetCurrentProcess(),
            addr,
            (ULONG64)offset,
            length,
            MEM_REPLACE_PLACEHOLDER,
            protect,
            nullptr,
            0);
        if (view == nullptr) {
            char _dbg_buf[256];
            snprintf(_dbg_buf, sizeof(_dbg_buf),
                     "mmap MAP_FIXED: MapViewOfFile3(addr=%p offset=%llu "
                     "len=%zu protect=%lu) failed: GetLastError=%lu\n",
                     addr, (unsigned long long)offset, length,
                     (unsigned long)protect,
                     (unsigned long)GetLastError());
            OutputDebugStringA(_dbg_buf);
            CloseHandle(mapping);
            errno = EACCES;
            return MAP_FAILED;
        }
        if (monad_compat_placeholder_table(addr, mapping, true) == nullptr) {
            apis->UnmapViewOfFile2(
                GetCurrentProcess(), addr, MEM_PRESERVE_PLACEHOLDER);
            CloseHandle(mapping);
            errno = ENOMEM;
            return MAP_FAILED;
        }
        return view;
    }
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
    // MapViewOfFile's offset must be aligned to the system allocation
    // granularity (64Kb on all current Windows architectures), unlike
    // mmap(2)'s page-size (4Kb) alignment requirement on Linux. Round the
    // requested offset down to the nearest 64Kb boundary and map the extra
    // leading bytes too, then return a pointer adjusted forward by the
    // difference so the caller sees its requested offset.
    uint64_t const granularity = 65536;
    uint64_t const aligned_offset = (uint64_t)offset & ~(granularity - 1);
    size_t const diff = (size_t)((uint64_t)offset - aligned_offset);
    void *const base = MapViewOfFile(
        mapping,
        access,
        (DWORD)(aligned_offset >> 32),
        (DWORD)(aligned_offset & 0xffffffffu),
        length + diff);
    if (base == nullptr) {
        CloseHandle(mapping);
        errno = EACCES;
        return MAP_FAILED;
    }
    void *const ret = (char *)base + diff;
    if (monad_compat_mmap_table(ret, mapping, base, nullptr, true) ==
        nullptr) {
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return ret;
}

static inline int munmap(void *const addr, size_t const length)
{
    void *real_base = addr;
    HANDLE const mapping =
        monad_compat_mmap_table(addr, nullptr, nullptr, &real_base, false);
    if (mapping != nullptr) {
        UnmapViewOfFile(real_base);
        CloseHandle(mapping);
        return 0;
    }

    // Not a file-backed mapping tracked above: this is a DbMetadataContext
    // placeholder-VA reservation (see mmap()'s MAP_ANONYMOUS|PROT_NONE
    // case), munmap()'d whole regardless of how many of its MAP_FIXED
    // chunk-slot views are still live. Revert each live view back to a
    // placeholder (closing its mapping handle) before releasing the entire
    // reservation -- VirtualFree(MEM_RELEASE) cannot release memory that
    // still holds a MapViewOfFile3 view.
    struct MonadPlaceholderVaApis const *const apis =
        monad_compat_placeholder_va_apis();
    size_t off = 0;
    while (off < length) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((char *)addr + off, &mbi, sizeof(mbi)) == 0) {
            break;
        }
        if (mbi.Type == MEM_MAPPED && apis->UnmapViewOfFile2 != nullptr) {
            HANDLE const view_mapping = monad_compat_placeholder_table(
                mbi.BaseAddress, nullptr, false);
            apis->UnmapViewOfFile2(
                GetCurrentProcess(), mbi.BaseAddress, MEM_PRESERVE_PLACEHOLDER);
            if (view_mapping != nullptr) {
                CloseHandle(view_mapping);
            }
        }
        off = (size_t)((char *)mbi.BaseAddress + mbi.RegionSize - (char *)addr);
    }
    VirtualFree(addr, 0, MEM_RELEASE);
    return 0;
}

// Open a memory buffer as a FILE stream, like glibc's fmemopen(); mingw has
// no equivalent, so this copies the buffer into an anonymous temporary file
// and returns a stream positioned at its start
static inline FILE *
fmemopen(void const *const buf, size_t const size, char const *const mode)
{
    (void)mode;
    FILE *const f = tmpfile();
    if (f == nullptr) {
        return nullptr;
    }
    if (size != 0 && fwrite(buf, 1, size, f) != size) {
        fclose(f);
        return nullptr;
    }
    rewind(f);
    return f;
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
    // FILE_SHARE_DELETE lets std::filesystem::remove() on this still-open
    // handle's path succeed (DeleteFileW opens its own handle requesting
    // delete access) -- storage_pool's anonymous-inode test fixtures call
    // current_path()+remove() on this file while it's still open, mirroring
    // POSIX's allow-unlink-of-open-file semantics. FILE_FLAG_DELETE_ON_CLOSE
    // already guarantees cleanup on close regardless. FILE_SHARE_READ is
    // additionally required so storage_pool::clone_as_read_only() can reopen
    // this same path with GENERIC_READ while this handle stays open --
    // Windows enforces sharing based on what the first handle allowed,
    // regardless of what the second open requests, mirroring POSIX's
    // open-for-read-while-open-for-read+write semantics.
    HANDLE const h = CreateFileA(
        tmp_file,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
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

// Positioned read/write that do not disturb the file descriptor's current
// position, like POSIX pread(2)/pwrite(2); implemented via ReadFile()/
// WriteFile() with an OVERLAPPED offset, which Win32 honours for synchronous
// handles too
static inline ssize_t
pread(int const fd, void *const buf, size_t const count, off_t const offset)
{
    HANDLE const h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)((uint64_t)offset & 0xffffffffu);
    ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD bytes_read = 0;
    if (!ReadFile(h, buf, (DWORD)count, &bytes_read, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF) {
            return 0;
        }
        errno = EIO;
        return -1;
    }
    return (ssize_t)bytes_read;
}

static inline ssize_t pwrite(
    int const fd, void const *const buf, size_t const count,
    off_t const offset)
{
    HANDLE const h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)((uint64_t)offset & 0xffffffffu);
    ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD bytes_written = 0;
    if (!WriteFile(h, buf, (DWORD)count, &bytes_written, &ov)) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)bytes_written;
}

static inline int fsync(int const fd)
{
    HANDLE const h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (!FlushFileBuffers(h)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static inline int msync(void *const addr, size_t const length, int const flags)
{
    (void)flags;
    // DbMetadataContext's ring spans are placeholder-VA reservations (see
    // mmap()'s MAP_ANONYMOUS|PROT_NONE and MAP_FIXED cases above): only some
    // sub-ranges are backed by individual MapViewOfFile3 views (one per
    // chunk slot), the rest remain PROT_NONE placeholders. Unlike Linux's
    // msync -- a no-op over anonymous pages, tolerant of a range spanning
    // multiple mappings -- FlushViewOfFile operates on a single view and
    // fails if given an address/length it doesn't own. Walk the region and
    // flush only the MEM_MAPPED sub-ranges, skipping placeholders.
    size_t off = 0;
    while (off < length) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((char *)addr + off, &mbi, sizeof(mbi)) == 0) {
            errno = EIO;
            return -1;
        }
        size_t const region_end = (size_t)(
            (char *)mbi.BaseAddress + mbi.RegionSize - (char *)addr);
        size_t const end = region_end < length ? region_end : length;
        if (mbi.Type == MEM_MAPPED) {
            if (!FlushViewOfFile((char *)addr + off, end - off)) {
                errno = EIO;
                return -1;
            }
        }
        off = end;
    }
    return 0;
}

// Win32 has no equivalent of madvise(); access pattern hints are purely
// advisory, so this is a no-op that always reports success
static inline int
madvise(void *const addr, size_t const length, int const advice)
{
    (void)addr;
    (void)length;
    (void)advice;
    return 0;
}

// pipe(2): mingw only provides _pipe(), which takes an explicit buffer size
// and text/binary mode flag
static inline int pipe(int pfds[2])
{
    return _pipe(pfds, 4096, _O_BINARY);
}

// flock(2): whole-file advisory lock via LockFileEx()/UnlockFileEx(), used to
// coordinate ownership of event-ring files between processes
static inline int flock(int const fd, int const operation)
{
    HANDLE const h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov = {};
    if (operation & LOCK_UN) {
        if (!UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov)) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    DWORD flags = (operation & LOCK_EX) ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (operation & LOCK_NB) {
        flags |= LOCKFILE_FAIL_IMMEDIATELY;
    }
    if (!LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov)) {
        errno = (operation & LOCK_NB) ? EWOULDBLOCK : EIO;
        return -1;
    }
    return 0;
}

// renameat2(2) with RENAME_NOREPLACE: Windows rename() already fails with
// EEXIST if the destination exists, which is exactly the NOREPLACE
// semantic the callers in this codebase rely on; the dirfd arguments are
// unused since only absolute paths with AT_FDCWD are passed
static inline int renameat2(
    int /* olddirfd */, char const *const oldpath, int /* newdirfd */,
    char const *const newpath, unsigned int /* flags */)
{
    return rename(oldpath, newpath);
}

    #ifdef __cplusplus
    }
    #endif

#endif // _WIN32

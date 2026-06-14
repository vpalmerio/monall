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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if __has_include(<stdbit.h>)
    #include <stdbit.h>
#else
    // C23 <stdbit.h> is not yet available in mingw-w64's GCC headers
    #define stdc_has_single_bit(x) ((x) != 0 && (((x) & ((x) - 1)) == 0))
#endif

#ifdef _WIN32
    #include <category/core/compat.h>
#else
    #include <sys/mman.h>
#endif

#include <category/core/event/event_iterator.h>
#include <category/core/event/event_recorder.h>
#include <category/core/event/event_ring.h>
#include <category/core/format_err.h>
#include <category/core/srcloc.h>

thread_local char _g_monad_event_ring_error_buf[1024];
static size_t const PAGE_2MB = 1UL << 21, HEADER_SIZE = PAGE_2MB;

#ifdef _WIN32

// VirtualAlloc2/MapViewOfFile3/UnmapViewOfFile2 implement the "placeholder
// VA" APIs (Windows 10 1803+) that this file uses to emulate the POSIX
// MAP_FIXED "wraparound" double-mapping trick for the payload buffer.
// mingw-w64's headers declare these (gated on NTDDI_VERSION >=
// NTDDI_WIN10_RS4), but its import libraries do not yet export them, so we
// resolve them dynamically from kernelbase.dll at runtime.
typedef PVOID(WINAPI *VirtualAlloc2_fn)(
    HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID(WINAPI *MapViewOfFile3_fn)(
    HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
    MEM_EXTENDED_PARAMETER *, ULONG);
typedef WINBOOL(WINAPI *UnmapViewOfFile2_fn)(HANDLE, PVOID, ULONG);

static VirtualAlloc2_fn g_VirtualAlloc2;
static MapViewOfFile3_fn g_MapViewOfFile3;
static UnmapViewOfFile2_fn g_UnmapViewOfFile2;

// Resolve the placeholder-VA APIs on first use; returns false if the running
// version of Windows does not provide them (pre-1803)
static bool resolve_placeholder_va_apis()
{
    if (g_VirtualAlloc2 && g_MapViewOfFile3 && g_UnmapViewOfFile2) {
        return true;
    }
    HMODULE const kernelbase = GetModuleHandleA("kernelbase.dll");
    if (kernelbase == nullptr) {
        return false;
    }
    g_VirtualAlloc2 =
        (VirtualAlloc2_fn)(void *)GetProcAddress(kernelbase, "VirtualAlloc2");
    g_MapViewOfFile3 = (MapViewOfFile3_fn)(void *)GetProcAddress(
        kernelbase, "MapViewOfFile3");
    g_UnmapViewOfFile2 = (UnmapViewOfFile2_fn)(void *)GetProcAddress(
        kernelbase, "UnmapViewOfFile2");
    return g_VirtualAlloc2 && g_MapViewOfFile3 && g_UnmapViewOfFile2;
}

#endif // _WIN32

#define FORMAT_ERRC(...)                                                       \
    monad_format_err(                                                          \
        _g_monad_event_ring_error_buf,                                         \
        sizeof(_g_monad_event_ring_error_buf),                                 \
        &MONAD_SOURCE_LOCATION_CURRENT(),                                      \
        __VA_ARGS__)

int monad_event_ring_init_size(
    uint8_t const descriptors_shift, uint8_t const payload_buf_shift,
    uint16_t const context_large_pages,
    struct monad_event_ring_size *const size)
{
    // Do some basic input validation of the size; our main goal here is to
    // protect the event ring from being too small as it can create certain
    // problems (e.g., the descriptor array extent being smaller than a single
    // large page, or problems with the buffer_window_start optimization if
    // WINDOW_INCR is too close in size to the shift, etc.).  While we are
    // here anyway, add some reasonable realistic maximums.
    if (descriptors_shift < MONAD_EVENT_MIN_DESCRIPTORS_SHIFT ||
        descriptors_shift > MONAD_EVENT_MAX_DESCRIPTORS_SHIFT) {
        return FORMAT_ERRC(
            ERANGE,
            "descriptors_shift %hhu outside allowed range [%hhu, %hhu]: "
            "(ring sizes: [%zu, %zu])",
            descriptors_shift,
            MONAD_EVENT_MIN_DESCRIPTORS_SHIFT,
            MONAD_EVENT_MAX_DESCRIPTORS_SHIFT,
            ((size_t)1 << MONAD_EVENT_MIN_DESCRIPTORS_SHIFT),
            ((size_t)1 << MONAD_EVENT_MAX_DESCRIPTORS_SHIFT));
    }
    if (payload_buf_shift < MONAD_EVENT_MIN_PAYLOAD_BUF_SHIFT ||
        payload_buf_shift > MONAD_EVENT_MAX_PAYLOAD_BUF_SHIFT) {
        return FORMAT_ERRC(
            ERANGE,
            "payload_buf_shift %hhu outside allowed range [%hhu, %hhu]: "
            "(buffer sizes: [%zu, %zu])",
            payload_buf_shift,
            MONAD_EVENT_MIN_PAYLOAD_BUF_SHIFT,
            MONAD_EVENT_MAX_PAYLOAD_BUF_SHIFT,
            ((size_t)1 << MONAD_EVENT_MIN_PAYLOAD_BUF_SHIFT),
            ((size_t)1 << MONAD_EVENT_MAX_PAYLOAD_BUF_SHIFT));
    }
    size->descriptor_capacity = (size_t)1 << descriptors_shift;
    size->payload_buf_size = (size_t)1 << payload_buf_shift;
    size->context_area_size = PAGE_2MB * context_large_pages;
    return 0;
}

size_t monad_event_ring_calc_storage(
    struct monad_event_ring_size const *const ring_size)
{
    return PAGE_2MB +
           ring_size->descriptor_capacity *
               sizeof(struct monad_event_descriptor) +
           ring_size->payload_buf_size + ring_size->context_area_size;
}

// A normal event ring is divided into sections, which are aligned to 2 MiB
// x64 large page boundaries:
//
//  .------------------.
//  |   Ring header    |
//  .------------------.
//  | Descriptor array |
//  .------------------.
//  |  Payload buffer  |
//  .------------------.
//  |   Context area   |
//  .------------------.
int monad_event_ring_init_file(
    struct monad_event_ring_size const *const ring_size,
    enum monad_event_content_type const content_type,
    uint8_t const *const schema_hash, int const ring_fd,
    off_t const ring_offset, char const *error_name)
{
    size_t ring_bytes;
    void *map_base;
    struct stat ring_stat;
    struct monad_event_ring_header header;
    char namebuf[64];

    if (error_name == nullptr) {
        snprintf(namebuf, sizeof namebuf, "fd:%d [%d]", ring_fd, getpid());
        error_name = namebuf;
    }

    // Do some basic input validation if they didn't obtain their size object
    // via a call to monad_event_ring_init_size
    if (!stdc_has_single_bit(ring_size->descriptor_capacity) ||
        ring_size->descriptor_capacity <
            ((size_t)1 << MONAD_EVENT_MIN_DESCRIPTORS_SHIFT) ||
        ring_size->descriptor_capacity >
            ((size_t)1 << MONAD_EVENT_MAX_DESCRIPTORS_SHIFT)) {
        return FORMAT_ERRC(
            EINVAL,
            "event ring file `%s` descriptor size %zu is invalid; use "
            "monad_event_ring_init_size",
            error_name,
            ring_size->descriptor_capacity);
    }
    if (!stdc_has_single_bit(ring_size->payload_buf_size) ||
        ring_size->payload_buf_size <
            ((size_t)1 << MONAD_EVENT_MIN_PAYLOAD_BUF_SHIFT) ||
        ring_size->payload_buf_size >
            ((size_t)1 << MONAD_EVENT_MAX_PAYLOAD_BUF_SHIFT)) {
        return FORMAT_ERRC(
            EINVAL,
            "event ring file `%s` descriptor size %zu is invalid; use "
            "monad_event_ring_init_size",
            error_name,
            ring_size->payload_buf_size);
    }
    if (ring_size->context_area_size > 0 &&
        !stdc_has_single_bit(ring_size->context_area_size)) {
        return FORMAT_ERRC(
            EINVAL,
            "event ring file `%s` context area size %zu is invalid",
            error_name,
            ring_size->context_area_size);
    }
    if (content_type == MONAD_EVENT_CONTENT_TYPE_NONE ||
        content_type >= MONAD_EVENT_CONTENT_TYPE_COUNT) {
        return FORMAT_ERRC(
            EINVAL,
            "event ring file `%s` has invalid content type code %hu",
            error_name,
            content_type);
    }

    memset(&header, 0, sizeof header);
    memcpy(header.magic, MONAD_EVENT_RING_HEADER_VERSION, sizeof header.magic);
    memcpy(header.schema_hash, schema_hash, sizeof header.schema_hash);
    header.content_type = content_type;
    header.size = *ring_size;
    ring_bytes = monad_event_ring_calc_storage(ring_size);

    // The caller is responsible for ensuring that the file range
    // [ring_offset, ring_offset+ring_bytes) is valid. We check that they've
    // done this, because we will mmap this range and need to be sure we won't
    // get SIGBUS upon access.
    if (fstat(ring_fd, &ring_stat) == -1) {
        return FORMAT_ERRC(
            errno, "unable to fstat event ring file `%s`", error_name);
    }
    if (ring_offset + (off_t)ring_bytes > ring_stat.st_size) {
        return FORMAT_ERRC(
            ENOSPC,
            "event ring file `%s` cannot hold total event ring size %zu",
            error_name,
            ring_bytes);
    }

    // Map the file and copy the header into it
    map_base =
        mmap(nullptr, ring_bytes, PROT_WRITE, MAP_SHARED, ring_fd, ring_offset);
    if (map_base == MAP_FAILED) {
        return FORMAT_ERRC(
            errno, "mmap failed for event ring file `%s`", error_name);
    }
    memcpy(map_base, &header, sizeof header);

    // To function correctly, all event descriptor's sequence number fields
    // need to be zero'ed out when creating a new event ring. Because we may
    // be given a file region that's already been used, memset the whole
    // descriptor region to zero.
    memset(
        (uint8_t *)map_base + HEADER_SIZE,
        0,
        sizeof(struct monad_event_descriptor) *
            header.size.descriptor_capacity);

    munmap(map_base, ring_bytes);
    return 0;
}

int monad_event_ring_mmap(
    struct monad_event_ring *const event_ring, int const mmap_prot,
    int const mmap_extra_flags, int const ring_fd, off_t const ring_offset,
    char const *error_name)
{
    int rc;
    char namebuf[64];
    struct monad_event_ring_header const *header;
    off_t const base_ring_data_offset = ring_offset + (off_t)PAGE_2MB;

    if (event_ring == nullptr) {
        return FORMAT_ERRC(EFAULT, "event_ring cannot be nullptr");
    }
    if (error_name == nullptr) {
        snprintf(namebuf, sizeof namebuf, "fd:%d [%d]", ring_fd, getpid());
        error_name = namebuf;
    }

    event_ring->mmap_prot = mmap_prot;
    header = event_ring->header = mmap(
        nullptr,
        HEADER_SIZE,
        mmap_prot,
        MAP_SHARED | mmap_extra_flags,
        ring_fd,
        ring_offset);
    if (header == MAP_FAILED) {
        return FORMAT_ERRC(
            errno, "mmap of event ring file `%s` header failed", error_name);
    }
    if (memcmp(
            header->magic,
            MONAD_EVENT_RING_HEADER_VERSION,
            sizeof header->magic) != 0) {
        return FORMAT_ERRC(
            EPROTO,
            "event ring file `%s` does not contain current magic number",
            error_name);
    }
    event_ring->desc_capacity_mask =
        event_ring->header->size.descriptor_capacity - 1;
    event_ring->payload_buf_mask =
        event_ring->header->size.payload_buf_size - 1;

    // Map the ring descriptor array from the ring fd
    size_t const descriptor_map_len = header->size.descriptor_capacity *
                                      sizeof(struct monad_event_descriptor);
    event_ring->descriptors = mmap(
        nullptr,
        descriptor_map_len,
        mmap_prot,
        MAP_SHARED | mmap_extra_flags,
        ring_fd,
        base_ring_data_offset);
    if (event_ring->descriptors == MAP_FAILED) {
        rc = FORMAT_ERRC(
            errno,
            "mmap of event ring file `%s` event descriptor array failed",
            error_name);
        goto Error;
    }

    // The mmap step of the payload buffer is more complex: first, reserve a
    // single anonymous mapping whose size is twice the size of the payload
    // buffer, so we can do the "wrap around" trick. We'll remap the actual
    // payload buffer fd into this reserved range later, using MAP_FIXED.
#ifdef _WIN32
    // On Windows we reproduce the same "wrap around" layout using the
    // placeholder VA APIs (Windows 10 1803+): reserve 2x payload_size, split
    // it into two placeholders, then replace each placeholder with a view of
    // the same file region via MapViewOfFile3/MEM_REPLACE_PLACEHOLDER.
    {
        size_t const payload_size = header->size.payload_buf_size;
        off_t const payload_offset =
            base_ring_data_offset + (off_t)descriptor_map_len;
        DWORD const payload_protect =
            (mmap_prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
        HANDLE payload_file_mapping;
        HANDLE const ring_file = (HANDLE)_get_osfhandle(ring_fd);

        if (!resolve_placeholder_va_apis()) {
            rc = FORMAT_ERRC(
                ENOSYS,
                "event ring file `%s`: this version of Windows does not "
                "provide the placeholder VA APIs (VirtualAlloc2/"
                "MapViewOfFile3) needed to map the payload buffer",
                error_name);
            goto Error;
        }
        if (ring_file == INVALID_HANDLE_VALUE) {
            rc = FORMAT_ERRC(
                EBADF,
                "event ring file `%s` does not have a valid Win32 handle",
                error_name);
            goto Error;
        }
        // MapViewOfFile3 with MEM_REPLACE_PLACEHOLDER requires the view
        // (payload_offset .. payload_offset + payload_size) to be fully
        // covered by the section; unlike mmap() on Linux, it does not
        // tolerate a view that extends a few bytes past EOF within the
        // final page. Decompressed snapshot files can be slightly shorter
        // than the ring's nominal total size (trailing zero bytes elided by
        // the snapshot writer), so grow the file here if needed.
        {
            LARGE_INTEGER cur_size;
            uint64_t const needed_size =
                (uint64_t)payload_offset + (uint64_t)payload_size;
            if (GetFileSizeEx(ring_file, &cur_size) &&
                (uint64_t)cur_size.QuadPart < needed_size) {
                LARGE_INTEGER new_size;
                new_size.QuadPart = (LONGLONG)needed_size;
                if (SetFilePointerEx(ring_file, new_size, nullptr, FILE_BEGIN)) {
                    SetEndOfFile(ring_file);
                }
            }
        }
        payload_file_mapping = CreateFileMappingA(
            ring_file, nullptr, payload_protect, 0, 0, nullptr);
        if (payload_file_mapping == nullptr) {
            rc = FORMAT_ERRC(
                EACCES,
                "CreateFileMapping for event ring file `%s` payload buffer "
                "failed",
                error_name);
            goto Error;
        }
        event_ring->payload_buf = g_VirtualAlloc2(
            nullptr,
            nullptr,
            2 * payload_size,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
            PAGE_NOACCESS,
            nullptr,
            0);
        if (event_ring->payload_buf == nullptr) {
            CloseHandle(payload_file_mapping);
            rc = FORMAT_ERRC(
                ENOMEM,
                "VirtualAlloc2 reservation for event ring file `%s` payload "
                "buffer failed",
                error_name);
            goto Error;
        }
        if (!VirtualFree(
                event_ring->payload_buf,
                payload_size,
                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            VirtualFree(event_ring->payload_buf, 0, MEM_RELEASE);
            event_ring->payload_buf = nullptr;
            CloseHandle(payload_file_mapping);
            rc = FORMAT_ERRC(
                ENOMEM,
                "splitting placeholder VA region for event ring file `%s` "
                "payload buffer failed",
                error_name);
            goto Error;
        }
        if (g_MapViewOfFile3(
                payload_file_mapping,
                nullptr,
                event_ring->payload_buf,
                (ULONG64)payload_offset,
                payload_size,
                MEM_REPLACE_PLACEHOLDER,
                payload_protect,
                nullptr,
                0) == nullptr) {
            VirtualFree(event_ring->payload_buf, 0, MEM_RELEASE);
            VirtualFree(event_ring->payload_buf + payload_size, 0, MEM_RELEASE);
            rc = FORMAT_ERRC(
                EACCES,
                "MapViewOfFile3 of event ring file `%s` payload buffer to %p "
                "failed",
                error_name,
                event_ring->payload_buf);
            event_ring->payload_buf = nullptr;
            CloseHandle(payload_file_mapping);
            goto Error;
        }
        if (g_MapViewOfFile3(
                payload_file_mapping,
                nullptr,
                event_ring->payload_buf + payload_size,
                (ULONG64)payload_offset,
                payload_size,
                MEM_REPLACE_PLACEHOLDER,
                payload_protect,
                nullptr,
                0) == nullptr) {
            g_UnmapViewOfFile2(
                GetCurrentProcess(),
                event_ring->payload_buf,
                MEM_PRESERVE_PLACEHOLDER);
            VirtualFree(event_ring->payload_buf, 0, MEM_RELEASE);
            VirtualFree(event_ring->payload_buf + payload_size, 0, MEM_RELEASE);
            rc = FORMAT_ERRC(
                EACCES,
                "MapViewOfFile3 of event ring file `%s` payload buffer "
                "wrap-around pages at %p failed",
                error_name,
                event_ring->payload_buf + payload_size);
            event_ring->payload_buf = nullptr;
            CloseHandle(payload_file_mapping);
            goto Error;
        }

        // The views maintain their own reference to the section object, so
        // the file mapping handle is no longer needed
        CloseHandle(payload_file_mapping);
    }
#else
    event_ring->payload_buf = mmap(
        nullptr,
        2 * header->size.payload_buf_size,
        mmap_prot,
        MAP_SHARED | MAP_ANONYMOUS | mmap_extra_flags,
        -1,
        base_ring_data_offset + (off_t)descriptor_map_len);
    if (event_ring->payload_buf == MAP_FAILED) {
        rc = FORMAT_ERRC(
            errno,
            "mmap of event ring file `%s` payload buffer anonymous region "
            "failed",
            error_name);
        goto Error;
    }

    // Map the payload buffer into the first part of the space we just reserved
    if (mmap(
            event_ring->payload_buf,
            header->size.payload_buf_size,
            mmap_prot,
            MAP_FIXED | MAP_SHARED | mmap_extra_flags,
            ring_fd,
            base_ring_data_offset + (off_t)descriptor_map_len) == MAP_FAILED) {
        rc = FORMAT_ERRC(
            errno,
            "fixed mmap of event ring file `%s` payload buffer to %p failed",
            error_name,
            event_ring->payload_buf);
        goto Error;
    }

    // Map the "wrap around" view of the payload buffer immediately after the
    // previous mapping. This allows memcpy(3) to naturally "wrap around" in
    // memory by the size of one maximally-sized event. Thus, we can copy event
    // payloads safely near the end of the buffer, without needing to do any
    // error-prone index massaging.
    if (mmap(
            event_ring->payload_buf + header->size.payload_buf_size,
            header->size.payload_buf_size,
            mmap_prot,
            MAP_FIXED | MAP_SHARED | mmap_extra_flags,
            ring_fd,
            base_ring_data_offset + (off_t)descriptor_map_len) == MAP_FAILED) {
        rc = FORMAT_ERRC(
            errno,
            "fixed mmap of event ring file `%s` payload buffer wrap-around "
            "pages at %p "
            "failed",
            error_name,
            event_ring->payload_buf + header->size.payload_buf_size);
        goto Error;
    }
#endif // _WIN32

    if (header->size.context_area_size > 0) {
        event_ring->context_area = mmap(
            nullptr,
            header->size.context_area_size,
            mmap_prot,
            MAP_SHARED | mmap_extra_flags,
            ring_fd,
            base_ring_data_offset +
                (off_t)(descriptor_map_len + header->size.payload_buf_size));
        if (event_ring->context_area == MAP_FAILED) {
            rc = FORMAT_ERRC(
                errno,
                "mmap of event ring file `%s` context area failed",
                error_name);
            goto Error;
        }
    }

    return 0;

Error:
    monad_event_ring_unmap(event_ring);
    return rc;
}

void monad_event_ring_unmap(struct monad_event_ring *const event_ring)
{
    struct monad_event_ring_header const *const header = event_ring->header;
    if (header != nullptr) {
        if (event_ring->descriptors) {
            munmap(
                event_ring->descriptors,
                header->size.descriptor_capacity *
                    sizeof(struct monad_event_descriptor));
        }
        if (event_ring->payload_buf) {
#ifdef _WIN32
            size_t const payload_size = header->size.payload_buf_size;
            // resolve_placeholder_va_apis() must succeed here, since it
            // already succeeded when this payload_buf was mapped
            if (resolve_placeholder_va_apis()) {
                g_UnmapViewOfFile2(
                    GetCurrentProcess(),
                    event_ring->payload_buf,
                    MEM_PRESERVE_PLACEHOLDER);
                g_UnmapViewOfFile2(
                    GetCurrentProcess(),
                    event_ring->payload_buf + payload_size,
                    MEM_PRESERVE_PLACEHOLDER);
            }
            VirtualFree(event_ring->payload_buf, 0, MEM_RELEASE);
            VirtualFree(event_ring->payload_buf + payload_size, 0, MEM_RELEASE);
#else
            munmap(event_ring->payload_buf, 2 * header->size.payload_buf_size);
#endif
        }
        if (event_ring->context_area) {
            munmap(event_ring->context_area, header->size.context_area_size);
        }
        munmap((void *)header, HEADER_SIZE);
    }
    memset(event_ring, 0, sizeof *event_ring);
}

int monad_event_ring_init_iterator(
    struct monad_event_ring const *const event_ring,
    struct monad_event_iterator *const iter)
{
    memset(iter, 0, sizeof *iter);
    struct monad_event_ring_header const *header = event_ring->header;
    if (header == nullptr) {
        return FORMAT_ERRC(EINVAL, "event_ring has been unmapped");
    }
    if ((event_ring->mmap_prot & PROT_READ) == 0) {
        return FORMAT_ERRC(EACCES, "event_ring memory not mapped for reading");
    }
    iter->descriptors = event_ring->descriptors;
    iter->desc_capacity_mask = header->size.descriptor_capacity - 1;
    iter->control = &header->control;
    (void)monad_event_iterator_reset(iter);
    return 0;
}

int monad_event_ring_init_recorder(
    struct monad_event_ring const *const event_ring,
    struct monad_event_recorder *const recorder)
{
    memset(recorder, 0, sizeof *recorder);
    struct monad_event_ring_header *header = event_ring->header;
    if (header == nullptr) {
        return FORMAT_ERRC(EINVAL, "event_ring has been unmapped");
    }
    if ((event_ring->mmap_prot & PROT_WRITE) == 0) {
        return FORMAT_ERRC(EACCES, "event_ring memory not mapped for writing");
    }
    recorder->descriptors = event_ring->descriptors;
    recorder->payload_buf = event_ring->payload_buf;
    recorder->control = &header->control;
    recorder->desc_capacity_mask = header->size.descriptor_capacity - 1;
    recorder->payload_buf_mask = header->size.payload_buf_size - 1;
    return 0;
}

char const *monad_event_ring_get_last_error()
{
    return _g_monad_event_ring_error_buf;
}

char const *g_monad_event_content_type_names[MONAD_EVENT_CONTENT_TYPE_COUNT] = {
    [MONAD_EVENT_CONTENT_TYPE_NONE] = "none",
    [MONAD_EVENT_CONTENT_TYPE_TEST] = "test",
    [MONAD_EVENT_CONTENT_TYPE_EXEC] = "exec",
};

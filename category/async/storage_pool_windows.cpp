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

// Windows placeholder for category/async/storage_pool.cpp. The real
// implementation relies on Linux block-device ioctls, zonefs, mmap-based
// on-disk metadata and O_TMPFILE/O_PATH anonymous inodes, none of which have
// equivalents on Windows. No Phase 2 test constructs a storage_pool -- these
// abort if ever reached. See the Windows port plan for the follow-up
// storage_pool functional port.

#include <category/async/storage_pool.hpp>

#include <category/core/assert.h>

MONAD_ASYNC_NAMESPACE_BEGIN

std::filesystem::path storage_pool::device_t::current_path() const
{
    MONAD_ABORT_PRINTF(
        "storage_pool::device_t::current_path not yet implemented on "
        "Windows");
}

size_t storage_pool::device_t::chunks() const
{
    MONAD_ABORT_PRINTF(
        "storage_pool::device_t::chunks not yet implemented on Windows");
}

size_t storage_pool::device_t::cnv_chunks() const
{
    MONAD_ABORT_PRINTF(
        "storage_pool::device_t::cnv_chunks not yet implemented on Windows");
}

std::pair<file_offset_t, file_offset_t> storage_pool::device_t::capacity() const
{
    MONAD_ABORT_PRINTF(
        "storage_pool::device_t::capacity not yet implemented on Windows");
}

storage_pool::chunk_t::~chunk_t() = default;

std::pair<int, file_offset_t>
storage_pool::chunk_t::write_fd(size_t const bytes_which_shall_be_written) noexcept
{
    (void)bytes_which_shall_be_written;
    MONAD_ABORT_PRINTF(
        "storage_pool::chunk_t::write_fd not yet implemented on Windows");
}

file_offset_t storage_pool::chunk_t::size() const
{
    MONAD_ABORT_PRINTF(
        "storage_pool::chunk_t::size not yet implemented on Windows");
}

void storage_pool::chunk_t::destroy_contents()
{
    MONAD_ABORT_PRINTF(
        "storage_pool::chunk_t::destroy_contents not yet implemented on "
        "Windows");
}

uint32_t
storage_pool::chunk_t::clone_contents_into(chunk_t &other, uint32_t const bytes)
{
    (void)other;
    (void)bytes;
    MONAD_ABORT_PRINTF(
        "storage_pool::chunk_t::clone_contents_into not yet implemented on "
        "Windows");
}

bool storage_pool::chunk_t::try_trim_contents(uint32_t const bytes)
{
    (void)bytes;
    MONAD_ABORT_PRINTF(
        "storage_pool::chunk_t::try_trim_contents not yet implemented on "
        "Windows");
}

storage_pool::device_t storage_pool::make_device_(
    mode const op, device_t::type_t_ const type,
    std::filesystem::path const &path, int const fd,
    std::variant<uint64_t, device_t const *> const dev_no_or_dev,
    creation_flags const flags)
{
    (void)op;
    (void)type;
    (void)path;
    (void)fd;
    (void)dev_no_or_dev;
    (void)flags;
    MONAD_ABORT_PRINTF(
        "storage_pool::make_device_ not yet implemented on Windows");
}

void storage_pool::fill_chunks_(creation_flags const &flags)
{
    (void)flags;
    MONAD_ABORT_PRINTF(
        "storage_pool::fill_chunks_ not yet implemented on Windows");
}

storage_pool::storage_pool(
    storage_pool const *const src, clone_as_read_only_tag_)
    : is_read_only_(true)
    , is_read_only_allow_dirty_(src->is_read_only_allow_dirty_)
    , is_migration_allowed_(src->is_migration_allowed_)
    , is_newly_truncated_(false)
{
    MONAD_ABORT_PRINTF(
        "storage_pool clone_as_read_only_tag_ constructor not yet "
        "implemented on Windows");
}

storage_pool::storage_pool(
    std::span<std::filesystem::path const> const sources, mode const mode,
    creation_flags const flags)
    : is_read_only_(flags.open_read_only || flags.open_read_only_allow_dirty)
    , is_read_only_allow_dirty_(flags.open_read_only_allow_dirty)
    , is_migration_allowed_(flags.allow_migration)
    , is_newly_truncated_(mode == mode::truncate)
{
    (void)sources;
    MONAD_ABORT_PRINTF(
        "storage_pool not yet implemented on Windows");
}

storage_pool::storage_pool(use_anonymous_inode_tag, creation_flags const flags)
    : is_read_only_(flags.open_read_only || flags.open_read_only_allow_dirty)
    , is_read_only_allow_dirty_(flags.open_read_only_allow_dirty)
    , is_migration_allowed_(flags.allow_migration)
    , is_newly_truncated_(false)
{
    MONAD_ABORT_PRINTF(
        "storage_pool not yet implemented on Windows");
}

storage_pool::storage_pool(
    use_anonymous_sized_inode_tag, off_t const len, creation_flags const flags)
    : is_read_only_(flags.open_read_only || flags.open_read_only_allow_dirty)
    , is_read_only_allow_dirty_(flags.open_read_only_allow_dirty)
    , is_migration_allowed_(flags.allow_migration)
    , is_newly_truncated_(false)
{
    (void)len;
    MONAD_ABORT_PRINTF(
        "storage_pool not yet implemented on Windows");
}

storage_pool::~storage_pool() = default;

storage_pool::chunk_t &storage_pool::chunk(chunk_type const which, uint32_t const id)
{
    (void)which;
    (void)id;
    MONAD_ABORT_PRINTF(
        "storage_pool::chunk not yet implemented on Windows");
}

storage_pool storage_pool::clone_as_read_only() const
{
    MONAD_ABORT_PRINTF(
        "storage_pool::clone_as_read_only not yet implemented on Windows");
}

storage_pool::chunk_t storage_pool::activate_chunk(
    chunk_type const which, device_t &device, uint32_t const id_within_device,
    uint32_t const id_within_zone)
{
    (void)which;
    (void)device;
    (void)id_within_device;
    (void)id_within_zone;
    MONAD_ABORT_PRINTF(
        "storage_pool::activate_chunk not yet implemented on Windows");
}

MONAD_ASYNC_NAMESPACE_END

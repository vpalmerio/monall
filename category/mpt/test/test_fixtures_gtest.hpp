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

#include "test_fixtures_base.hpp"

#include <category/async/test/test_fixture.hpp>
#include <category/mpt/db.hpp>
#include <category/mpt/ondisk_db_config.hpp>

namespace monad::test
{
    struct InMemoryMerkleTrieGTest
        : public MerkleTrie<InMemoryTrieBase<::testing::Test>>
    {
        using MerkleTrie<InMemoryTrieBase<::testing::Test>>::InMemoryTrieBase;
    };

    struct OnDiskMerkleTrieGTest
        : public MerkleTrie<OnDiskTrieBase<::testing::Test>>
    {
        using MerkleTrie<OnDiskTrieBase<::testing::Test>>::OnDiskTrieBase;
    };

    struct InMemoryTrieGTest
        : public PlainTrie<InMemoryTrieBase<::testing::Test>>
    {
    };

    struct OnDiskTrieGTest : public PlainTrie<OnDiskTrieBase<::testing::Test>>
    {
    };

    template <FillDBWithChunksConfig Config>
    struct FillDBWithChunksGTest
        : public FillDBWithChunks<Config, ::testing::Test>
    {
        using FillDBWithChunks<Config, ::testing::Test>::FillDBWithChunks;
    };

    inline std::filesystem::path create_temp_file(long const size_gb)
    {
        auto const [fd, filename] = MONAD_ASYNC_NAMESPACE::make_temp_file(
            MONAD_ASYNC_NAMESPACE::working_temporary_directory() /
            "monad_db_test_XXXXXX");
        MONAD_ASSERT(fd != -1);
        // Use resize_file() instead of ::ftruncate() for two reasons:
        // 1. On Windows, MinGW's ftruncate() routes through msvcrt's _chsize_s
        //    which looks up the fd in msvcrt's CRT fd table.  make_temp_file()
        //    uses _open_osfhandle (ucrtbase) → ucrtbase's table → EBADF from
        //    msvcrt.  resize_file() uses _get_osfhandle (ucrtbase) to recover
        //    the Win32 HANDLE and calls SetEndOfFile directly, bypassing the
        //    fd-table mismatch entirely.
        // 2. Using int64_t avoids the 32-bit overflow on Windows LLP64 that
        //    would silently truncate e.g. 4 GiB to 0 bytes.
        MONAD_ASSERT(
            -1
            != MONAD_ASYNC_NAMESPACE::resize_file(
                   fd, int64_t(size_gb) * 1024 * 1024 * 1024));
        ::close(fd);
        return filename;
    }

    template <class StateMachineType>
        requires std::is_base_of_v<StateMachine, StateMachineType>
    struct OnDiskDbWithFileFixtureBase : public ::testing::Test
    {
        std::filesystem::path const dbname;
        monad::mpt::OnDiskDbConfig config;
        monad::mpt::Db db;
        Node::SharedPtr root;

        OnDiskDbWithFileFixtureBase()
            : dbname{create_temp_file(8)}
            , config{OnDiskDbConfig{
                  .compaction = true,
                  .sq_thread_cpu = std::nullopt,
                  .dbname_paths = {dbname},
                  .fixed_history_length = MPT_TEST_HISTORY_LENGTH}}
            , db{std::make_unique<StateMachineType>(), config}
            , root{}
        {
        }

        ~OnDiskDbWithFileFixtureBase()
        {
            std::filesystem::remove(dbname);
        }
    };

}

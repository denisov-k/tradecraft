// Copyright (c) 2021 The Bitcoin Core developers
// Copyright (c) 2011-2024 The Freicoin Developers
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of version 3 of the GNU Affero General Public License as published
// by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#ifndef FREICOIN_NODE_CACHES_H
#define FREICOIN_NODE_CACHES_H

#include <kernel/caches.h>
#include <util/byte_units.h>

#include <cstddef>

class ArgsManager;

//! min. -dbcache (bytes)
static constexpr size_t MIN_DB_CACHE{4_MiB};
//! -dbcache default (bytes)
static constexpr size_t DEFAULT_DB_CACHE{DEFAULT_KERNEL_CACHE};

namespace node {
size_t GetDefaultDBCache();
struct IndexCacheSizes {
    size_t tx_index{0};
    size_t filter_index{0};
    size_t txospender_index{0};
};
struct CacheSizes {
    IndexCacheSizes index;
    kernel::CacheSizes kernel;
};
CacheSizes CalculateCacheSizes(const ArgsManager& args, size_t n_indexes = 0);
constexpr bool ShouldWarnOversizedDbCache(size_t dbcache, size_t total_ram) noexcept
{
    const size_t cap{(total_ram < 2048_MiB) ? DEFAULT_DB_CACHE : (total_ram / 100) * 75};
    return dbcache > cap;
}

void LogOversizedDbCache(const ArgsManager& args) noexcept;
} // namespace node

#endif // FREICOIN_NODE_CACHES_H

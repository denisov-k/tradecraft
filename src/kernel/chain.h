// Copyright (c) 2022 The Bitcoin Core developers
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

#ifndef FREICOIN_KERNEL_CHAIN_H
#define FREICOIN_KERNEL_CHAIN_H

#include <attributes.h>

#include <iostream>

class CBlock;
class CBlockIndex;
class CBlockUndo;
class uint256;

namespace interfaces {
//! Block data sent with blockConnected, blockDisconnected notifications.
struct BlockInfo {
    const uint256& hash;
    const uint256* prev_hash = nullptr;
    int height = -1;
    int file_number = -1;
    unsigned data_pos = 0;
    const CBlock* data = nullptr;
    const CBlockUndo* undo_data = nullptr;
    // The maximum time in the chain up to and including this block.
    // A timestamp that can only move forward.
    unsigned int chain_time_max{0};

    BlockInfo(const uint256& hash LIFETIMEBOUND) : hash(hash) {}
};
} // namespace interfaces

namespace kernel {
struct ChainstateRole;
//! Return data from block index.
interfaces::BlockInfo MakeBlockInfo(const CBlockIndex* block_index, const CBlock* data = nullptr);
std::ostream& operator<<(std::ostream& os, const ChainstateRole& role);
} // namespace kernel

#endif // FREICOIN_KERNEL_CHAIN_H

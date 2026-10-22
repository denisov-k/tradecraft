// Copyright (c) 2023 The Freicoin Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_SHARECHAINSTATEMANAGER_OPTS_H
#define BITCOIN_KERNEL_SHARECHAINSTATEMANAGER_OPTS_H

#include <util/time.h>

#include <cstdint>
#include <optional>
#include <uint256.h>
#include <functional>

class CBlockHeader;
class CChainParams;
struct ShareChainParams;

namespace kernel {

/**
 * @brief An options struct for `ShareChainstateManager`, more ergonomically
 * referred to as `ShareChainstateManager::Options` due to the
 * using-declaration in `ShareChainstateManager`.
 */
struct ShareChainstateManagerOpts {
    const CChainParams& chainparams;
    const ShareChainParams& sharechainparams;
    const std::function<NodeClock::time_point()> adjusted_time_callback{nullptr};
    //! Height of a known block hash, or nullopt (template-age rule).
    std::function<std::optional<int>(const uint256&)> block_height_callback{nullptr};
    //! Height of the active block chain tip, or -1 when the chain is not loaded yet.
    std::function<int()> tip_height_callback{nullptr};
    //! The nBits a block with this header must carry on top of the given
    //! previous block, or nullopt if that block is unknown (window-size rule).
    std::function<std::optional<uint32_t>(const uint256&, const CBlockHeader&)> required_bits_callback{nullptr};
};

} // namespace kernel

#endif // BITCOIN_KERNEL_SHARECHAINSTATEMANAGER_OPTS_H

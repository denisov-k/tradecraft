// Copyright (c) 2026 The Freicoin Developers
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

#ifndef FREICOIN_CONSENSUS_ASSETS_H
#define FREICOIN_CONSENSUS_ASSETS_H

#include <consensus/amount.h>
#include <uint256.h>

#include <cstddef>
#include <vector>

class CCoinsViewCache;
class COutPoint;
class CTransaction;
class CTxOut;
class TxValidationState;

/** Extension outputs.  A native witness output may end in one data push after
 *  its program (and shard specifier), which consensus ignored until now; with
 *  the assets deployment its first byte is the extension version. */
static constexpr unsigned char EXTENSION_ASSET = 0x01;
/** An asset output's extension: version, asset id, amount (64-bit little endian). */
static constexpr size_t ASSET_EXTENSION_SIZE = 1 + 32 + 8;

/** An amount of a user-issued asset, held in a zero-value output.  Assets carry
 *  no demurrage: the amount is the amount, whatever the reference height. */
struct AssetAmount {
    uint256 asset;
    CAmount amount{0};
};

enum class AssetOutputType {
    NONE,      //!< no asset extension (any other output, or an unknown version)
    ASSET,     //!< a well-formed asset extension
    MALFORMED, //!< an asset extension of the wrong size
};

/** Read the asset extension of an output, if it has one. */
AssetOutputType GetAssetOutput(const CTxOut& txout, AssetAmount& out);

/** The extension push of an asset output: version, asset id, amount. */
std::vector<unsigned char> AssetExtension(const AssetAmount& amount);

/** The asset a transaction issues: derived from the outpoint its first input
 *  spends, which can be spent once, so every asset is issued exactly once. */
uint256 AssetIssuanceId(const COutPoint& prevout);

namespace Consensus {
/** Check the asset rules of a transaction: asset outputs are well formed and
 *  hold no freicoin, and for every asset the outputs carry no more than the
 *  inputs, except the asset the transaction issues.  Inputs created below
 *  `asset_height`, the activation height, never had their claims checked and
 *  count for nothing.  The coins spent must be in `inputs`. */
[[nodiscard]] bool CheckTxAssets(const CTransaction& tx, const CCoinsViewCache& inputs, int asset_height, TxValidationState& state);
} // namespace Consensus

#endif // FREICOIN_CONSENSUS_ASSETS_H

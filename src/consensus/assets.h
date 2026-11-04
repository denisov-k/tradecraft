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
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <cstddef>
#include <utility>
#include <vector>

class BlockValidationState;
class CBlock;
class CCoinsViewCache;
class Coin;
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
[[nodiscard]] bool CheckTxAssets(const CTransaction& tx, const CCoinsViewCache& inputs, int asset_height, TxValidationState& state, bool block_final = false);
} // namespace Consensus

/** The scrip ledger (Forward Blocks, section XI): scrip is freicoin valued at the
 *  scrip epoch (FreicoinToScrip), an asset without demurrage, backed by a pool of
 *  freicoin held in the block-final transaction.  Freicoin enters the pool through
 *  peg-in outputs, which the block-final transaction of the same block claims, and
 *  leaves it through peg-outs, which the coinbase of the same block pays: to nodes
 *  that do not know these rules the pool is a trivially spendable output and the
 *  payouts are fees, so this is a soft fork. */
static constexpr unsigned char EXTENSION_POOL = 0x02;   //!< the pool: scrip outstanding
static constexpr unsigned char EXTENSION_PEGIN = 0x03;  //!< freicoin paid into the pool
static constexpr unsigned char EXTENSION_PEGOUT = 0x04; //!< scrip paid out to the output's program
/** A ledger output's extension: version, amount (64-bit little endian). */
static constexpr size_t LEDGER_EXTENSION_SIZE = 1 + 8;

/** The asset id of scrip. */
uint256 ScripAssetId();
/** The witness program of the pool and of peg-ins: an unassigned 2-byte version 0
 *  program, which anyone can spend, so the block-final transaction can. */
CScript LedgerBase();

enum class LedgerOutputType {
    NONE,
    POOL,      //!< amount: the scrip outstanding; value: the freicoin backing it
    PEGIN,     //!< value: the freicoin paid in
    PEGOUT,    //!< amount: the scrip paid out, to the output's witness program
    MALFORMED, //!< a ledger extension of the wrong size, or a pool or peg-in on another program
};

/** Read the ledger extension of an output, if it has one. */
LedgerOutputType GetLedgerOutput(const CTxOut& txout, CAmount& amount);

/** The extension push of a ledger output. */
std::vector<unsigned char> LedgerExtension(unsigned char version, CAmount amount);

/** The ledger movements of one block, gathered as its transactions are seen. */
struct BlockLedger {
    CAmount prior_value{0};   //!< the prior pool, at its present value
    CAmount prior_scrip{0};   //!< scrip outstanding before the block
    CAmount pegin_value{0};   //!< freicoin paid in, at its present value
    CAmount minted{0};        //!< scrip minted by the peg-ins
    std::vector<COutPoint> pegins;
    std::vector<std::pair<CScript, CAmount>> pegouts; //!< destination program, scrip

    /** A prior block-final output: the pool, if it is one. */
    void AddPriorOutput(const Coin& coin, int height);
    /** A transaction of the block (not the coinbase or the block-final one). */
    void AddTransaction(const CTransaction& tx, int height);
    bool Active() const { return prior_value || prior_scrip || !pegins.empty() || !pegouts.empty(); }
};

/** The pool after a block and the payouts its coinbase owes.  Each peg-out is paid
 *  ScripToFreicoin of its scrip at the block height, out of the pool and the block's
 *  peg-ins, in order; a payout the pool cannot cover in full gets what is left (the
 *  pool keeps every rounding, so only the last kria of the last scrip could). */
struct LedgerResult {
    CAmount pool_value{0};
    CAmount pool_scrip{0};
    std::vector<std::pair<CScript, CAmount>> payouts; //!< destination program, freicoin
};
LedgerResult ComputeLedger(const BlockLedger& ledger, int height);

namespace Consensus {
/** Check a block against its ledger movements: the block-final transaction claims
 *  every peg-in and holds exactly the pool ComputeLedger gives (before its last
 *  output, the commitments), and the coinbase pays every payout. */
[[nodiscard]] bool CheckBlockLedger(const CBlock& block, const BlockLedger& ledger, int height, bool has_final_tx, BlockValidationState& state);
} // namespace Consensus

#endif // FREICOIN_CONSENSUS_ASSETS_H

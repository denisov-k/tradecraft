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
/** An asset output's extension: version, asset id, amount (64-bit little endian),
 *  and for an asset with demurrage the flag byte. */
static constexpr size_t ASSET_EXTENSION_SIZE = 1 + 32 + 8;
static constexpr size_t ASSET_EXTENSION_SIZE_DEMURRAGE = ASSET_EXTENSION_SIZE + 1;
static constexpr unsigned char ASSET_DEMURRAGE = 0x01;

/** An amount of a user-issued asset, held in a zero-value output.  By default an
 *  asset carries no demurrage: the amount is the amount, whatever the reference
 *  height.  An asset issued with demurrage decays exactly as freicoin does: the
 *  amount is its value at the output's reference height (the lock height of its
 *  transaction).  Which it is is fixed when the asset is issued (the id commits to
 *  it), so a claim on freicoin can be an asset that keeps pace with freicoin. */
struct AssetAmount {
    uint256 asset;
    CAmount amount{0};
    bool demurrage{false};
};

/** The value of an asset amount `distance` blocks after its reference height: with
 *  demurrage, freicoin's (GetTimeAdjustedValue); without, the amount. */
CAmount AssetPresentValue(CAmount amount, bool demurrage, uint32_t distance);

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
 *  spends, which can be spent once, so every asset is issued exactly once, and
 *  from whether it carries demurrage, which is thus fixed for good. */
uint256 AssetIssuanceId(const COutPoint& prevout, bool demurrage = false);

namespace Consensus {
/** Check the asset rules of a transaction: asset outputs are well formed and
 *  hold no freicoin, and for every asset the outputs carry no more than the
 *  inputs, valued at the transaction's lock height, except the assets the
 *  transaction issues; an asset's outputs and inputs all agree on its demurrage.  Inputs created below
 *  `asset_height`, the activation height, never had their claims checked and
 *  count for nothing.  The coins spent must be in `inputs`. */
[[nodiscard]] bool CheckTxAssets(const CTransaction& tx, const CCoinsViewCache& inputs, int asset_height, TxValidationState& state, bool block_final = false);
} // namespace Consensus

/** The scrip ledger (Forward Blocks, section XI): scrip is freicoin valued at the
 *  scrip epoch (FreicoinToScrip), an asset without demurrage, backed by a pool of
 *  freicoin held in the block-final transaction.  Freicoin enters the pool through
 *  peg-in outputs, which the block-final transaction of the same block claims, and
 *  leaves it through peg-outs, which the coinbase of the block `ledger_payout_delay`
 *  later pays (a payout queue: what a block owes follows from the chain below it,
 *  so a share of the share chain can be checked, and a reorg near the tip cannot
 *  move a payout).  To nodes that do not know these rules the pool is a trivially
 *  spendable output and the payouts are fees, so this is a soft fork.
 *
 *  A fee may be paid in scrip (Forward Blocks XI-B, the explicit fee): a fee output
 *  burns scrip, and the block's pool gives up its freicoin value, which reaches the
 *  coinbase as a fee of the block-final transaction, like any implicit fee. */
static constexpr unsigned char EXTENSION_POOL = 0x02;   //!< the pool: scrip outstanding, at most
static constexpr unsigned char EXTENSION_PEGIN = 0x03;  //!< freicoin paid into the pool
static constexpr unsigned char EXTENSION_PEGOUT = 0x04; //!< scrip paid out to the output's program
static constexpr unsigned char EXTENSION_FEE = 0x05;    //!< scrip paid as a fee to the miner
/** A ledger output's extension: version, amount (64-bit little endian). */
static constexpr size_t LEDGER_EXTENSION_SIZE = 1 + 8;

/** The asset id of scrip. */
uint256 ScripAssetId();
/** The witness program of the pool and of peg-ins: an unassigned 2-byte version 0
 *  program, which anyone can spend, so the block-final transaction can. */
CScript LedgerBase();

enum class LedgerOutputType {
    NONE,
    POOL,      //!< amount: the scrip the peg-ins could mint less the peg-outs, a bound on the scrip outstanding; value: the freicoin backing it
    PEGIN,     //!< value: the freicoin paid in
    PEGOUT,    //!< amount: the scrip paid out, to the output's witness program
    FEE,       //!< amount: the scrip paid as a fee
    MALFORMED, //!< a ledger extension of the wrong size, or a pool, peg-in or fee on another program
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
    CAmount fee_scrip{0};     //!< scrip paid as fees
    std::vector<COutPoint> pegins;
    //! The peg-outs this block pays: those of the block ledger_payout_delay below (destination program, scrip)
    std::vector<std::pair<CScript, CAmount>> pegouts;

    /** A prior block-final output: the pool, if it is one (and was made since activation). */
    void AddPriorOutput(const Coin& coin, int height, int asset_height);
    /** A transaction of the block (not the coinbase or the block-final one): its peg-ins and fees. */
    void AddTransaction(const CTransaction& tx, int height);
    bool Active() const { return prior_value || prior_scrip || !pegins.empty() || !pegouts.empty() || fee_scrip; }
};

/** The pool after a block and the payouts its coinbase owes.  Each peg-out is paid
 *  ScripToFreicoin of its scrip at the block height, out of the prior pool, in order
 *  (the block's own peg-ins join the pool after); a payout the pool cannot cover in
 *  full gets what is left (the pool keeps every rounding, so only the last kria of the
 *  last scrip could).  Then the fees paid in scrip leave the pool, peg-ins included,
 *  at their freicoin value at the block height. */
struct LedgerResult {
    CAmount pool_value{0};
    CAmount pool_scrip{0};
    CAmount fee_value{0}; //!< the freicoin the fees in scrip forward to the coinbase
    std::vector<std::pair<CScript, CAmount>> payouts; //!< destination program, freicoin
};
LedgerResult ComputeLedger(const BlockLedger& ledger, int height);

/** The freicoin value at `height` of the fees a transaction pays in scrip (its fee
 *  outputs), which reaches the coinbase through the pool: what relay and mining policy
 *  add to its freicoin fee.  Out-of-range fee outputs, which make the transaction
 *  invalid, count for nothing. */
CAmount LedgerFeeValue(const CTransaction& tx, int height);

/** The peg-outs a block makes, in order: what the block ledger_payout_delay above it pays. */
std::vector<std::pair<CScript, CAmount>> BlockPegouts(const CBlock& block);

namespace Consensus {
/** Check a block against its ledger movements: the block-final transaction claims
 *  every peg-in and holds exactly the pool ComputeLedger gives (before its last
 *  output, the commitments), and the coinbase pays every payout. */
[[nodiscard]] bool CheckBlockLedger(const CBlock& block, const BlockLedger& ledger, int height, bool has_final_tx, BlockValidationState& state);
} // namespace Consensus

#endif // FREICOIN_CONSENSUS_ASSETS_H

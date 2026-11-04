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

#include <consensus/assets.h>

#include <coins.h>
#include <primitives/block.h>
#include <consensus/validation.h>
#include <crypto/common.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <tinyformat.h>

#include <algorithm>
#include <map>
#include <set>

AssetOutputType GetAssetOutput(const CTxOut& txout, AssetAmount& out)
{
    std::vector<unsigned char> ext;
    if (!txout.scriptPubKey.GetExtensionOutput(ext) || ext.empty() || ext[0] != EXTENSION_ASSET) {
        return AssetOutputType::NONE;
    }
    if (ext.size() != ASSET_EXTENSION_SIZE) {
        return AssetOutputType::MALFORMED;
    }
    std::copy(ext.begin() + 1, ext.begin() + 33, out.asset.begin());
    out.amount = static_cast<CAmount>(ReadLE64(ext.data() + 33));
    return AssetOutputType::ASSET;
}

std::vector<unsigned char> AssetExtension(const AssetAmount& amount)
{
    std::vector<unsigned char> ext(ASSET_EXTENSION_SIZE);
    ext[0] = EXTENSION_ASSET;
    std::copy(amount.asset.begin(), amount.asset.end(), ext.begin() + 1);
    WriteLE64(ext.data() + 33, static_cast<uint64_t>(amount.amount));
    return ext;
}

uint256 AssetIssuanceId(const COutPoint& prevout)
{
    static const HashWriter HASHER_ISSUANCE{TaggedHash("Freicoin/asset-issuance")};
    return (HashWriter{HASHER_ISSUANCE} << prevout).GetSHA256();
}

uint256 ScripAssetId()
{
    static const uint256 id{(HashWriter{TaggedHash("Freicoin/scrip")}).GetSHA256()};
    return id;
}

CScript LedgerBase()
{
    return CScript() << OP_0 << std::vector<unsigned char>{'F', 'S'};
}

LedgerOutputType GetLedgerOutput(const CTxOut& txout, CAmount& amount)
{
    std::vector<unsigned char> ext;
    if (!txout.scriptPubKey.GetExtensionOutput(ext) || ext.empty()) {
        return LedgerOutputType::NONE;
    }
    LedgerOutputType type;
    switch (ext[0]) {
        case EXTENSION_POOL: type = LedgerOutputType::POOL; break;
        case EXTENSION_PEGIN: type = LedgerOutputType::PEGIN; break;
        case EXTENSION_PEGOUT: type = LedgerOutputType::PEGOUT; break;
        default: return LedgerOutputType::NONE;
    }
    if (ext.size() != LEDGER_EXTENSION_SIZE) {
        return LedgerOutputType::MALFORMED;
    }
    // the pool and the peg-ins must be spendable by the block-final transaction
    if (type != LedgerOutputType::PEGOUT && txout.scriptPubKey.WithoutExtension() != LedgerBase()) {
        return LedgerOutputType::MALFORMED;
    }
    amount = static_cast<CAmount>(ReadLE64(ext.data() + 1));
    return type;
}

std::vector<unsigned char> LedgerExtension(unsigned char version, CAmount amount)
{
    std::vector<unsigned char> ext(LEDGER_EXTENSION_SIZE);
    ext[0] = version;
    WriteLE64(ext.data() + 1, static_cast<uint64_t>(amount));
    return ext;
}

void BlockLedger::AddPriorOutput(const Coin& coin, int height)
{
    CAmount scrip = 0;
    if (coin.IsSpent() || GetLedgerOutput(coin.out, scrip) != LedgerOutputType::POOL) return;
    prior_value += coin.GetPresentValue(height);
    prior_scrip += scrip;
}

void BlockLedger::AddTransaction(const CTransaction& tx, int height)
{
    for (uint32_t n = 0; n < tx.vout.size(); ++n) {
        const CTxOut& txout = tx.vout[n];
        CAmount amount = 0;
        switch (GetLedgerOutput(txout, amount)) {
            case LedgerOutputType::PEGIN:
                pegins.emplace_back(tx.GetHash(), n);
                pegin_value += GetTimeAdjustedValue(txout.GetReferenceValue(), height - static_cast<int>(tx.lock_height));
                minted += FreicoinToScrip(txout.GetReferenceValue(), tx.lock_height);
                break;
            case LedgerOutputType::PEGOUT:
                pegouts.emplace_back(txout.scriptPubKey.WithoutExtension(), amount);
                break;
            default:
                break;
        }
    }
}

LedgerResult ComputeLedger(const BlockLedger& ledger, int height)
{
    LedgerResult result;
    CAmount available = ledger.prior_value + ledger.pegin_value;
    result.pool_scrip = ledger.prior_scrip + ledger.minted;
    for (const auto& [dest, scrip] : ledger.pegouts) {
        const CAmount pay = std::min(ScripToFreicoin(scrip, height), available);
        available -= pay;
        result.pool_scrip -= scrip;
        result.payouts.emplace_back(dest, pay);
    }
    result.pool_value = available;
    return result;
}

bool Consensus::CheckBlockLedger(const CBlock& block, const BlockLedger& ledger, int height, bool has_final_tx, BlockValidationState& state)
{
    if (!ledger.Active()) return true;
    if (!has_final_tx) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-blk-ledger-no-final-tx");
    }
    const CTransaction& final_tx = *block.vtx.back();
    std::set<COutPoint> claimed;
    for (const CTxIn& txin : final_tx.vin) claimed.insert(txin.prevout);
    for (const COutPoint& pegin : ledger.pegins) {
        if (!claimed.count(pegin)) {
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-blk-ledger-pegin-unclaimed");
        }
    }
    const LedgerResult result = ComputeLedger(ledger, height);
    if (result.pool_scrip < 0) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-blk-ledger-pool-negative");
    }
    // the pool: one output (before the commitments in the last), or none when empty
    size_t pools = 0;
    for (size_t n = 0; n < final_tx.vout.size(); ++n) {
        CAmount scrip = 0;
        if (GetLedgerOutput(final_tx.vout[n], scrip) != LedgerOutputType::POOL) continue;
        ++pools;
        if (n + 1 == final_tx.vout.size() || final_tx.vout[n].GetReferenceValue() != result.pool_value || scrip != result.pool_scrip) {
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-blk-ledger-pool",
                                 strprintf("pool %d kria for %d scrip, expected %d for %d", final_tx.vout[n].GetReferenceValue(), scrip, result.pool_value, result.pool_scrip));
        }
    }
    if (pools != ((result.pool_value || result.pool_scrip) ? 1U : 0U)) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-blk-ledger-pool-count");
    }
    // the payouts: each by its own coinbase output
    const CTransaction& coinbase = *block.vtx[0];
    std::vector<bool> used(coinbase.vout.size(), false);
    for (const auto& [dest, pay] : result.payouts) {
        if (pay <= 0) continue;
        bool paid = false;
        for (size_t n = 0; n < coinbase.vout.size() && !paid; ++n) {
            if (!used[n] && coinbase.vout[n].scriptPubKey == dest && coinbase.vout[n].GetReferenceValue() >= pay) {
                used[n] = paid = true;
            }
        }
        if (!paid) {
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-blk-ledger-payout");
        }
    }
    return true;
}

bool Consensus::CheckTxAssets(const CTransaction& tx, const CCoinsViewCache& inputs, int asset_height, TxValidationState& state, bool block_final)
{
    std::map<uint256, CAmount> value_out;
    CAmount minted = 0;
    for (const CTxOut& txout : tx.vout) {
        CAmount ledger_amount = 0;
        const LedgerOutputType ledger = GetLedgerOutput(txout, ledger_amount);
        if (ledger == LedgerOutputType::MALFORMED) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ledger-malformed");
        }
        if (ledger != LedgerOutputType::NONE && tx.IsCoinBase()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ledger-coinbase");
        }
        if (ledger == LedgerOutputType::POOL && !block_final) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ledger-pool");
        }
        if (ledger == LedgerOutputType::PEGIN) {
            if (txout.GetReferenceValue() <= 0 || ledger_amount != 0) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-pegin-value");
            }
            minted += FreicoinToScrip(txout.GetReferenceValue(), tx.lock_height);
            continue;
        }
        if (ledger == LedgerOutputType::PEGOUT) {
            if (txout.GetReferenceValue() != 0) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-pegout-nonzero-value");
            }
            if (ledger_amount <= 0 || ledger_amount > MAX_MONEY) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-pegout-amount-outofrange");
            }
            CAmount& sum = value_out[ScripAssetId()];
            sum += ledger_amount;
            if (!MoneyRange(sum)) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-outputs-outofrange");
            }
            continue;
        }
        AssetAmount a;
        const AssetOutputType type = GetAssetOutput(txout, a);
        if (type == AssetOutputType::NONE) continue;
        if (type == AssetOutputType::MALFORMED) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-malformed");
        }
        // The freicoin of an asset output would be spendable by whoever holds the
        // asset; keep the two ledgers apart.
        if (txout.GetReferenceValue() != 0) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-nonzero-value");
        }
        if (a.amount <= 0 || a.amount > MAX_MONEY) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-amount-outofrange");
        }
        CAmount& sum = value_out[a.asset];
        sum += a.amount;
        if (!MoneyRange(sum)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-outputs-outofrange");
        }
    }
    if (tx.IsCoinBase()) {
        if (!value_out.empty()) return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-coinbase");
        return true;
    }

    std::map<uint256, CAmount> value_in;
    if (minted) value_in[ScripAssetId()] = minted;
    for (const CTxIn& txin : tx.vin) {
        const Coin& coin = inputs.AccessCoin(txin.prevout);
        // A claim made before the rules activated was never checked: it is worthless.
        if (static_cast<int>(coin.nHeight) < asset_height) continue;
        CAmount ledger_amount = 0;
        switch (GetLedgerOutput(coin.out, ledger_amount)) {
            case LedgerOutputType::PEGOUT:
                // its freicoin went to the coinbase; the output is spent for good
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-pegout-spend");
            case LedgerOutputType::PEGIN:
            case LedgerOutputType::POOL:
                if (!block_final) return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ledger-spend");
                continue;
            default:
                break;
        }
        AssetAmount a;
        if (GetAssetOutput(coin.out, a) != AssetOutputType::ASSET) continue;
        CAmount& sum = value_in[a.asset];
        sum += a.amount;
        if (!MoneyRange(sum)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-inputs-outofrange");
        }
    }

    // An asset input with no asset output is burned, which needs no check.
    const uint256 issued = AssetIssuanceId(tx.vin[0].prevout);
    for (const auto& [asset, amount] : value_out) {
        if (asset == issued) continue; // the issuance: any supply, once
        const auto it = value_in.find(asset);
        if (it == value_in.end() || it->second < amount) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-in-belowout",
                                 strprintf("asset %s: in %d < out %d", asset.ToString(), it == value_in.end() ? 0 : it->second, amount));
        }
    }
    return true;
}

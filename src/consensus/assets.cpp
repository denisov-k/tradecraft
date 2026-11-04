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
#include <consensus/validation.h>
#include <crypto/common.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <tinyformat.h>

#include <map>

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

bool Consensus::CheckTxAssets(const CTransaction& tx, const CCoinsViewCache& inputs, int asset_height, TxValidationState& state)
{
    std::map<uint256, CAmount> value_out;
    for (const CTxOut& txout : tx.vout) {
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
    // An asset input with no asset output is burned, which needs no check.
    if (value_out.empty()) return true;
    if (tx.IsCoinBase()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-coinbase");
    }

    std::map<uint256, CAmount> value_in;
    for (const CTxIn& txin : tx.vin) {
        const Coin& coin = inputs.AccessCoin(txin.prevout);
        // A claim made before the rules activated was never checked: it is worthless.
        if (static_cast<int>(coin.nHeight) < asset_height) continue;
        AssetAmount a;
        if (GetAssetOutput(coin.out, a) != AssetOutputType::ASSET) continue;
        CAmount& sum = value_in[a.asset];
        sum += a.amount;
        if (!MoneyRange(sum)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-inputs-outofrange");
        }
    }

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

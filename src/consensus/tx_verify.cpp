// Copyright (c) 2017-2021 The Bitcoin Core developers
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

#include <consensus/tx_verify.h>

#include <chain.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/asset.h>
#include <consensus/harberger.h>

#include <map>
#include <set>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <hash.h>
#include <pubkey.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <util/check.h>
#include <util/moneystr.h>

bool IsFinalTx(const CTransaction &tx, int32_t nBlockHeight, int64_t nBlockTime)
{
    if (static_cast<int64_t>(tx.lock_height) > static_cast<int64_t>(nBlockHeight))
        return false;
    if (tx.nLockTime == 0)
        return true;
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;

    // Even if tx.nLockTime isn't satisfied by nBlockHeight/nBlockTime, a
    // transaction is still considered final if all inputs' nSequence ==
    // SEQUENCE_FINAL (0xffffffff), in which case nLockTime is ignored.
    //
    // Because of this behavior OP_CHECKLOCKTIMEVERIFY/CheckLockTime() will
    // also check that the spending input's nSequence != SEQUENCE_FINAL,
    // ensuring that an unsatisfied nLockTime value will actually cause
    // IsFinalTx() to return false here:
    for (const auto& txin : tx.vin) {
        if (!(txin.nSequence == CTxIn::SEQUENCE_FINAL))
            return false;
    }
    return true;
}

std::pair<int, int64_t> CalculateSequenceLocks(const CTransaction &tx, int flags, std::vector<int>& prevHeights, const CBlockIndex& block)
{
    assert(prevHeights.size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // Bitcoin also has the requirement that tx.nVersion be not 0 or
    // 1, as a soft-fork upgrade protection.  We don't have the same
    // requirement.
    bool fEnforceBIP68 = (flags & LOCKTIME_VERIFY_SEQUENCE) != 0;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68) {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
        const CTxIn& txin = tx.vin[txinIndex];

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            // The height of this input is not relevant for sequence locks
            prevHeights[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = prevHeights[txinIndex];

        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
            const int64_t nCoinTime{Assert(block.GetAncestor(std::max(nCoinHeight - 1, 0)))->GetMedianTimePast()};
            // NOTE: Subtract 1 to maintain nLockTime semantics
            // BIP 68 relative lock times have the semantics of calculating
            // the first block or time at which the transaction would be
            // valid. When calculating the effective block time or height
            // for the entire transaction, we switch to using the
            // semantics of nLockTime which is the last invalid block
            // time or height.  Thus we subtract 1 from the calculated
            // time or height.

            // Time-based relative lock-times are measured from the
            // smallest allowed timestamp of the block containing the
            // txout being spent, which is the median time past of the
            // block prior.
            nMinTime = std::max(nMinTime, nCoinTime + (int64_t)((txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1);
        } else {
            nMinHeight = std::max(nMinHeight, nCoinHeight + (int)(txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}

bool EvaluateSequenceLocks(const CBlockIndex& block, std::pair<int, int64_t> lockPair)
{
    assert(block.pprev);
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const CTransaction &tx, int flags, std::vector<int>& prevHeights, const CBlockIndex& block)
{
    return EvaluateSequenceLocks(block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    for (const auto& txin : tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (const auto& txout : tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const CCoinsViewCache& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const CTxOut &prevout = coin.out;
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

int64_t GetTransactionSigOpCost(const CTransaction& tx, const CCoinsViewCache& inputs, script_verify_flags flags)
{
    int64_t nSigOps = GetLegacySigOpCount(tx) * WITNESS_SCALE_FACTOR;

    if (tx.IsCoinBase())
        return nSigOps;

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs) * WITNESS_SCALE_FACTOR;
    }
    return nSigOps;
}

bool Consensus::CheckTxInputs(const CTransaction& tx, TxValidationState& state, const CCoinsViewCache& inputs, const Consensus::Params& params, int per_input_adjustment, int nSpendHeight, Consensus::RuleSet rules, CAmount& txfee, const Consensus::AssetRegistry* registry, const Consensus::NameRegistry* names)
{
    // are the actual inputs available?
    if (!inputs.HaveInputs(tx)) {
        return state.Invalid(TxValidationResult::TX_MISSING_INPUTS, "bad-txns-inputs-missingorspent",
                         strprintf("%s: inputs missing/spent", __func__));
    }

    // nVersion=3-lite: nExpireTime — the tx may not be included once the chain passes this height
    // (0 = never expires). The mirror of nLockTime; the primitive behind expiring offers.
    if (tx.nExpireTime != 0 && nSpendHeight > 0 && (uint32_t)nSpendHeight > tx.nExpireTime) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-expired",
            strprintf("tx expired (nExpireTime %u < height %d)", tx.nExpireTime, nSpendHeight));
    }

    // nVersion=3 DEX: the bundle partition must be sane (non-empty bundles that fit inside
    // vin/vout), and every bundle must be unexpired — a maker's stale offer only invalidates
    // a composite that INCLUDES it. The per-asset conservation below runs over the flat
    // transaction, so composites inherit every balance rule unchanged.
    if (tx.version == NV3_TX_VERSION && (!tx.bundles.empty() || !tx.ranged.empty())) {
        uint64_t bin = 0, bout = 0;
        for (const CBundle& b : tx.bundles) {
            if (b.nIn == 0 || b.nOut == 0) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-bundle-empty");
            }
            bin += b.nIn; bout += b.nOut;
            if (b.nExpireTime != 0 && nSpendHeight > 0 && (uint32_t)nSpendHeight > b.nExpireTime) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-bundle-expired",
                    strprintf("bundle expired (nExpireTime %u < height %d)", b.nExpireTime, nSpendHeight));
            }
        }
        // ranged bundles (2b) follow the fixed ones: nIn inputs, exactly two outputs each
        for (const CRangedBundle& r : tx.ranged) {
            if (r.nIn == 0 || r.priceNum == 0 || r.priceDen == 0 || r.minFill < 0 || r.maxFill < r.minFill) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ranged-descriptor");
            }
            bin += r.nIn; bout += 2;
            if (r.nExpireTime != 0 && nSpendHeight > 0 && (uint32_t)nSpendHeight > r.nExpireTime) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-bundle-expired",
                    strprintf("ranged bundle expired (nExpireTime %u < height %d)", r.nExpireTime, nSpendHeight));
            }
        }
        if (bin > tx.vin.size() || bout > tx.vout.size()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-bundle-partition");
        }
    }

    // nVersion=3-lite: balances are tallied PER ASSET (keyed by the 20-byte tag; the null tag is
    // the host currency). With no registry every output is the host currency, so this reduces
    // exactly to the single-asset rule below. Each asset's demurrage rate comes from the registry.
    // This tx may DEFINE a new asset (an OP_RETURN definition); that asset is minted from
    // nothing, so its params come from the definition and its balance rule is exempted below.
    const auto def = Consensus::ParseAssetDefinition(tx);
    const bool has_minted = def.has_value();
    const uint160 minted = has_minted ? def->first : uint160();
    // A definition may not REDEFINE an existing id — the def bytes are public once issued, so
    // accepting a re-publication would let anyone mint more of somebody else's asset — and it
    // must actually MINT (at least one output of the new asset), else it's pure registry spam.
    // Mirrors nv3chain.mjs ("asset already defined" / "definition mints nothing").
    if (has_minted) {
        if (registry && registry->IsKnown(minted)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-redefined");
        }
        bool mints = false;
        for (const CTxOut& o : tx.vout) {
            if (o.assetTag == minted) { mints = true; break; }
        }
        if (!mints) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-mints-nothing");
        }
    }
    auto params_of = [&](const uint160& tag) -> Consensus::AssetParams {
        if (tag.IsNull()) return Consensus::AssetParams{20, false, 1};
        if (has_minted && tag == minted) return def->second;
        return registry ? registry->Get(tag) : Consensus::AssetParams{};
    };
    auto asset_known = [&](const uint160& tag) {
        return tag.IsNull() || (has_minted && tag == minted) || (registry && registry->IsKnown(tag));
    };
    // Present value of one coin at tx.lock_height under its asset's own monetary policy:
    // demurrage melts at 2^-shift per block, interest (a bond) grows at 2^-shift per block
    // saturating at MAX_MONEY.
    auto asset_pv = [&](const uint160& tag, CAmount value, uint32_t dist) -> CAmount {
        const Consensus::AssetParams ap = params_of(tag);
        return ap.interest ? TimeAdjustValueForwardInterestK(value, dist, ap.shift)
                           : TimeAdjustValueForwardK(value, dist, ap.shift);
    };

    std::map<uint160, CAmount> in_pv;   // present value of inputs, per asset, at tx.lock_height
    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        const COutPoint &prevout = tx.vin[i].prevout;
        const Coin& coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        // If prev is coinbase, check that it's matured
        if (coin.IsCoinBase() && nSpendHeight - coin.nHeight < ((rules & Consensus::SIZE_EXPANSION) ? 1 : COINBASE_MATURITY)) {
            return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND, "bad-txns-premature-spend-of-coinbase",
                strprintf("tried to spend coinbase at depth %d", nSpendHeight - coin.nHeight));
        }

        // Check that lock_height is monotonically increasing.
        // This restriction is removed for zero-valued inputs in the protocol cleanup.
        if ((coin.out.GetReferenceValue() || !(rules & Consensus::PROTOCOL_CLEANUP)) && !params.bitcoin_mode && (tx.lock_height < coin.refheight)) {
            return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND, "bad-txns-non-monotonic-lock-height",
                strprintf("tx.lock_height < coin.refheight (%d < %d)", tx.lock_height, coin.refheight));
        }

        const uint160& tag = coin.out.assetTag;
        if (!asset_known(tag)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-unknown-asset");
        }
        // Check for negative or overflow input values. Present value uses the asset's own rate.
        CAmount nInput = asset_pv(tag, coin.out.GetReferenceValue(), (uint32_t)(tx.lock_height - coin.refheight)) + per_input_adjustment;
        CAmount& acc = in_pv[tag];
        acc += nInput;
        if (!MoneyRange(coin.out.GetReferenceValue()) || !MoneyRange(nInput) || !MoneyRange(acc)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-inputvalues-outofrange");
        }
    }

    // nVersion=3 DEX 2b: the miner materialized each ranged bundle's [payout, change] — check
    // them against the maker-signed descriptor. Give coins must be one asset; the fill is the
    // present value parted with:  fill = givePV(lock_height) − change.value.
    if (tx.version == NV3_TX_VERSION && !tx.ranged.empty()) {
        size_t in0 = 0, out0 = 0;
        for (const CBundle& b : tx.bundles) { in0 += b.nIn; out0 += b.nOut; }
        for (const CRangedBundle& r : tx.ranged) {
            const CTxOut& pay = tx.vout[out0];
            const CTxOut& change = tx.vout[out0 + 1];
            const uint160 give_asset = inputs.AccessCoin(tx.vin[in0].prevout).out.assetTag;
            CAmount give_pv = 0;
            for (size_t i = in0; i < in0 + r.nIn; ++i) {
                const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
                if (coin.out.assetTag != give_asset) {
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ranged-mixed-give");
                }
                give_pv += asset_pv(give_asset, coin.out.GetReferenceValue(), (uint32_t)(tx.lock_height - coin.refheight));
            }
            // nVersion=3 EXTENSION-OUTPUT: the asset tag rides in the output's scriptPubKey, so
            // compare the BASE program (tag stripped) against the descriptor's script and the
            // DERIVED tag against the descriptor's asset — the abstract comparison the model makes.
            if (pay.assetTag != r.payoutAsset || pay.scriptPubKey.GetWitnessBase() != r.payoutScript
                || change.assetTag != give_asset || change.scriptPubKey.GetWitnessBase() != r.changeScript) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ranged-destination");
            }
            const CAmount fill = give_pv - change.GetReferenceValue();
            if (fill < r.minFill || fill > r.maxFill) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ranged-fill-bounds");
            }
            // rounding favors the maker: 128-bit cross-multiply avoids overflow
            if ((unsigned __int128)pay.GetReferenceValue() * r.priceDen < (unsigned __int128)fill * r.priceNum) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ranged-price");
            }
            in0 += r.nIn; out0 += 2;
        }
    }

    // Freiland Harberger covenant (docs/freiland-covenant-spec.md §4, path A). Once HARBERGER is
    // active, a HRBG input — an anyone-can-spend witness-v2 output to old nodes — may only be spent
    // as a FORCED BUY: pay V = asset_pv(deposit) to the committed owner AND re-create a successor
    // HRBG output for the same name with value >= V. Both together stop a free acquisition (host
    // FRC is fungible, so paying only the owner could be sourced from the name's own deposit).
    if (rules & Consensus::HARBERGER) {
        unsigned int n_hrbg_in = 0;
        for (unsigned int i = 0; i < tx.vin.size(); ++i) {
            const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
            Consensus::HarbergerCovenant in_cov;
            if (!Consensus::ParseHarbergerOutput(coin.out.scriptPubKey, in_cov)) continue;
            // One HRBG input per tx for now (multiple would need positional payout/successor
            // matching to avoid one output satisfying several inputs — a later step).
            if (++n_hrbg_in > 1) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-harberger-multiple-inputs");
            }
            // V = the deposit's present value at this tx's lock_height (host demurrage, shift 20).
            // The (uint32_t) cast is safe for any FUNDED name: GetReferenceValue()>0 means the
            // monotonic-lock-height check above (lock_height >= refheight) applied, so distance >= 0.
            // A zero-value HRBG coin can skip that check under PROTOCOL_CLEANUP and underflow the
            // distance, but asset_pv(_, 0, _) == 0 ⇒ V == 0 ⇒ it only lets an already-worthless
            // (fully-lapsed) name be taken for 0, never a theft of a funded name. (audit 2026-07-24)
            const CAmount V = asset_pv(coin.out.assetTag, coin.out.GetReferenceValue(), (uint32_t)(tx.lock_height - coin.refheight));
            // (1) an output pays >= V host FRC to the owner (0014{owner})
            const CScript pay_script = CScript() << OP_0 << std::vector<unsigned char>(in_cov.owner.begin(), in_cov.owner.end());
            bool paid = false;
            for (const CTxOut& o : tx.vout) {
                if (o.assetTag.IsNull() && o.scriptPubKey == pay_script && o.GetReferenceValue() >= V) { paid = true; break; }
            }
            if (!paid) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-harberger-unpaid");
            }
            // (2) a successor HRBG output for the SAME name with value >= V (the deposit carries)
            bool successor = false;
            for (const CTxOut& o : tx.vout) {
                Consensus::HarbergerCovenant out_cov;
                if (Consensus::ParseHarbergerOutput(o.scriptPubKey, out_cov)
                    && out_cov.nameHash == in_cov.nameHash && o.GetReferenceValue() >= V) { successor = true; break; }
            }
            if (!successor) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-harberger-no-successor");
            }
        }

        // Name UNIQUENESS: creating a HRBG output for name N is valid only if N is free, OR this tx
        // spends N's current live holder (a transfer/revalue replacing it). Also: at most one HRBG
        // output per name within a single tx. Needs the name registry reflecting the pre-tx state.
        std::set<COutPoint> hrbg_inputs;
        for (const CTxIn& in : tx.vin) {
            if (Consensus::IsHarbergerOutput(inputs.AccessCoin(in.prevout).out.scriptPubKey)) hrbg_inputs.insert(in.prevout);
        }
        std::set<uint256> names_out;
        for (const CTxOut& o : tx.vout) {
            Consensus::HarbergerCovenant cov;
            if (!Consensus::ParseHarbergerOutput(o.scriptPubKey, cov)) continue;
            if (!names_out.insert(cov.nameHash).second) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-harberger-dup-name");
            }
            if (names && names->IsLive(cov.nameHash) && !hrbg_inputs.count(names->Get(cov.nameHash))) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-harberger-name-taken");
            }
        }
    }

    // Outputs are minted at tx.lock_height, so their present value equals their nominal value.
    // Range checks (bad-txns-vout-*) already ran in CheckTransaction before this method.
    std::map<uint160, CAmount> out_sum;
    for (const CTxOut& o : tx.vout) {
        if (!asset_known(o.assetTag)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-unknown-asset");
        }
        const uint64_t g = params_of(o.assetTag).granularity;
        if (g > 1 && (o.GetReferenceValue() % (CAmount)g) != 0) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-granularity");
        }
        out_sum[o.assetTag] += o.GetReferenceValue();
    }

    // Per-asset balance: for each asset, inputs' present value must cover the outputs. The host
    // currency leaves the miner fee; every other asset must be conserved exactly.
    std::set<uint160> tags;
    for (const auto& kv : in_pv) tags.insert(kv.first);
    for (const auto& kv : out_sum) tags.insert(kv.first);
    CAmount txfee_aux = 0;
    for (const uint160& tag : tags) {
        if (has_minted && tag == minted) continue;   // the newly-defined asset is minted from nothing
        const CAmount in = in_pv.count(tag) ? in_pv[tag] : 0;
        const CAmount out = out_sum.count(tag) ? out_sum[tag] : 0;
        if (out > in) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-in-belowout",
                strprintf("value in (%s) < value out (%s)", FormatMoney(in), FormatMoney(out)));
        }
        if (tag.IsNull()) {
            txfee_aux = in - out;   // the fee is denominated in the host currency
        } else if (in != out) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-not-conserved");
        }
    }
    if (!MoneyRange(txfee_aux)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-fee-outofrange");
    }

    // nVersion=3 EXTENSION-OUTPUT tokens (TWO-SIDED REVEAL): unique tokens are conserved per asset —
    // every output token must come from an input of the SAME asset (or be minted by a definition
    // tx), and no token may appear in two outputs. The chainstate keeps only each coin's 32-byte
    // token-set COMMITMENT (CTxOut::tokenCommit, derived from its scriptPubKey), never the tokens.
    // So both the input token sets (of the committed coins being spent) and the output token sets
    // (of this tx's committed outputs) are REVEALED in a single OP_RETURN "FRT1" payload and checked
    // here against the relevant commitment. Wire-supplied CTxOut::tokens are IGNORED — the reveal is
    // the sole authority. Mirrors core/nv3wire.mjs + core/nv3chain.mjs.
    std::map<uint32_t, std::vector<std::vector<unsigned char>>> out_reveal, in_reveal;
    if (!Consensus::ParseTokenReveal(tx, out_reveal, in_reveal)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-reveal");
    }
    // input tokens: from the verified input reveal, checked against each spent coin's commitment.
    std::set<std::pair<uint160, std::vector<unsigned char>>> input_tokens;
    for (uint32_t j = 0; j < tx.vin.size(); ++j) {
        const Coin& coin = inputs.AccessCoin(tx.vin[j].prevout);
        auto it = in_reveal.find(j);
        if (!coin.out.tokenCommit.IsNull()) {
            if (it == in_reveal.end()) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-input-unrevealed");
            }
            if (Consensus::TokenSetHash(it->second) != coin.out.tokenCommit) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-input-mismatch");
            }
            for (const auto& tok : it->second) input_tokens.emplace(coin.out.assetTag, tok);
        } else if (it != in_reveal.end()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-input-uncommitted");
        }
    }
    // output tokens: from the verified output reveal, enforcing commit↔reveal correspondence, then
    // the uniqueness + conservation checks.
    std::set<std::pair<uint160, std::vector<unsigned char>>> seen_out;
    for (uint32_t i = 0; i < tx.vout.size(); ++i) {
        const CTxOut& o = tx.vout[i];
        auto it = out_reveal.find(i);
        if (o.tokenCommit.IsNull()) {
            if (it != out_reveal.end()) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-output-uncommitted");
            }
            continue;
        }
        if (it == out_reveal.end()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-output-unrevealed");
        }
        if (Consensus::TokenSetHash(it->second) != o.tokenCommit) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-output-mismatch");
        }
        for (const auto& tok : it->second) {
            auto key = std::make_pair(o.assetTag, tok);
            if (!seen_out.insert(key).second) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-duplicate");
            }
            if (input_tokens.count(key) == 0 && !(has_minted && o.assetTag == minted)) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-token-created");
            }
        }
    }

    // nVersion=3-lite: authorizers. If a moved asset's definition names an authorizer, the tx
    // must carry that authorizer's ECDSA approval — a DER signature over
    // SHA256d("FRAPPROV" || txid || tag). Approvals ride witness-side (outside the txid), so
    // the signature is not circular; the txid commits to every output, tag, token and expiry.
    // Minting is exempt: the issuer chooses the authorizer in the definition itself.
    for (const uint160& tag : tags) {
        if (tag.IsNull() || (has_minted && tag == minted)) continue;
        const Consensus::AssetParams ap = params_of(tag);
        if (ap.authorizer.empty()) continue;
        const auto appr = std::find_if(tx.approvals.begin(), tx.approvals.end(),
                                       [&](const auto& a) { return a.first == tag; });
        if (appr == tx.approvals.end()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-not-authorized");
        }
        HashWriter ss{};
        ss << std::span{Consensus::ASSET_APPROVAL_TAG};
        ss << tx.GetHash();
        ss << tag;
        const CPubKey pubkey(ap.authorizer);
        if (!pubkey.IsFullyValid() || !pubkey.Verify(ss.GetHash(), appr->second)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-authorization-invalid");
        }
    }

    txfee = txfee_aux;
    return true;
}

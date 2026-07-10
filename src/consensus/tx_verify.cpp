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

#include <map>
#include <set>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include <consensus/validation.h>
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

bool Consensus::CheckTxInputs(const CTransaction& tx, TxValidationState& state, const CCoinsViewCache& inputs, const Consensus::Params& params, int per_input_adjustment, int nSpendHeight, Consensus::RuleSet rules, CAmount& txfee, const Consensus::AssetRegistry* registry)
{
    // are the actual inputs available?
    if (!inputs.HaveInputs(tx)) {
        return state.Invalid(TxValidationResult::TX_MISSING_INPUTS, "bad-txns-inputs-missingorspent",
                         strprintf("%s: inputs missing/spent", __func__));
    }

    // nVersion=3-lite: balances are tallied PER ASSET (keyed by the 20-byte tag; the null tag is
    // the host currency). With no registry every output is the host currency, so this reduces
    // exactly to the single-asset rule below. Each asset's demurrage rate comes from the registry.
    auto asset_known = [&](const uint160& tag) { return tag.IsNull() || (registry && registry->IsKnown(tag)); };
    auto asset_shift = [&](const uint160& tag) -> unsigned { return registry ? registry->Get(tag).shift : 20; };

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
        CAmount nInput = TimeAdjustValueForwardK(coin.out.GetReferenceValue(), (uint32_t)(tx.lock_height - coin.refheight), asset_shift(tag)) + per_input_adjustment;
        CAmount& acc = in_pv[tag];
        acc += nInput;
        if (!MoneyRange(coin.out.GetReferenceValue()) || !MoneyRange(nInput) || !MoneyRange(acc)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-inputvalues-outofrange");
        }
    }

    // Outputs are minted at tx.lock_height, so their present value equals their nominal value.
    // Range checks (bad-txns-vout-*) already ran in CheckTransaction before this method.
    std::map<uint160, CAmount> out_sum;
    for (const CTxOut& o : tx.vout) {
        if (!asset_known(o.assetTag)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-unknown-asset");
        }
        const uint64_t g = registry ? registry->Get(o.assetTag).granularity : 1;
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

    txfee = txfee_aux;
    return true;
}

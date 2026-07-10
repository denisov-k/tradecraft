// Copyright (c) 2011-2024 The Freicoin Developers
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of version 3 of the GNU Affero General Public License as published
// by the Free Software Foundation.  See <https://www.gnu.org/licenses/>.

// nVersion=3-lite: per-asset CheckTxInputs. Verifies that balances are tallied per asset —
// the host currency leaves the fee, a user asset must be conserved in present value, inflation
// and unknown assets are rejected, and with no registry a non-host input is unknown.

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/asset.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <chainparams.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <vector>

BOOST_FIXTURE_TEST_SUITE(asset_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(per_asset_check_tx_inputs)
{
    const Consensus::Params& consensus = Params().GetConsensus();

    // A community currency: shift=18 (melts faster than the host currency's shift=20).
    std::vector<unsigned char> def(2 + 8 + 32, 0);
    def[0] = 18; // shift
    const uint160 coop = Consensus::AssetIdFromDef(def);
    Consensus::AssetRegistry reg;
    reg.Define(coop, Consensus::AssetParams{18, false, 1});

    CCoinsView base;
    CCoinsViewCache view(&base);

    const uint32_t refheight = 1000;
    const CAmount amt = 100000000;
    auto add = [&](const uint160& tag) {
        const COutPoint op(Txid::FromUint256(m_rng.rand256()), 0);
        CTxOut out(amt, CScript() << OP_TRUE);
        out.assetTag = tag;
        view.AddCoin(op, Coin(out, refheight, 1, false), false);
        return op;
    };
    const COutPoint opHost = add(uint160{});   // host-currency coin (pays the fee)
    const COutPoint opCoop = add(coop);        // coop coin

    const uint32_t H = 1500;
    const CAmount coopPv = TimeAdjustValueForwardK(amt, H - refheight, 18);
    const CAmount hostPv = TimeAdjustValueForwardK(amt, H - refheight, 20);

    auto make = [&](CAmount coopOut, CAmount hostOut, const uint160& coopTag) {
        CMutableTransaction mtx;
        mtx.version = 3;
        mtx.lock_height = H;
        mtx.vin.emplace_back(opHost);
        mtx.vin.emplace_back(opCoop);
        CTxOut o0(coopOut, CScript() << OP_TRUE); o0.assetTag = coopTag;
        CTxOut o1(hostOut, CScript() << OP_TRUE);   // host currency (null tag)
        mtx.vout = {o0, o1};
        return CTransaction(mtx);
    };

    // valid: coop conserved at present value; host currency leaves a 2000-kria fee
    {
        const CTransaction tx = make(coopPv, hostPv - 2000, coop);
        TxValidationState state; CAmount fee = -1;
        BOOST_CHECK(Consensus::CheckTxInputs(tx, state, view, consensus, 0, (int)H + 100, Consensus::NONE, fee, &reg));
        BOOST_CHECK_EQUAL(fee, 2000);
    }
    // inflation: outputting the full coop nominal exceeds its melted present value -> rejected
    {
        const CTransaction tx = make(amt, hostPv - 2000, coop);
        TxValidationState state; CAmount fee = -1;
        BOOST_CHECK(!Consensus::CheckTxInputs(tx, state, view, consensus, 0, (int)H + 100, Consensus::NONE, fee, &reg));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-in-belowout");
    }
    // unknown asset: an unregistered tag on the coop output -> rejected
    {
        std::vector<unsigned char> other(2 + 8 + 32, 0); other[0] = 19;
        const uint160 bogus = Consensus::AssetIdFromDef(other);
        const CTransaction tx = make(coopPv, hostPv - 2000, bogus);
        TxValidationState state; CAmount fee = -1;
        BOOST_CHECK(!Consensus::CheckTxInputs(tx, state, view, consensus, 0, (int)H + 100, Consensus::NONE, fee, &reg));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-unknown-asset");
    }
    // no registry: a non-host input is unknown -> rejected (host-only mode)
    {
        const CTransaction tx = make(coopPv, hostPv - 2000, coop);
        TxValidationState state; CAmount fee = -1;
        BOOST_CHECK(!Consensus::CheckTxInputs(tx, state, view, consensus, 0, (int)H + 100, Consensus::NONE, fee, nullptr));
    }
}

BOOST_AUTO_TEST_SUITE_END()

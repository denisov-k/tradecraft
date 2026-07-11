// Copyright (c) 2011-2024 The Freicoin Developers
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of version 3 of the GNU Affero General Public License as published
// by the Free Software Foundation.  See <https://www.gnu.org/licenses/>.

// nVersion=3-lite: per-asset CheckTxInputs. Verifies that balances are tallied per asset —
// the host currency leaves the fee, a user asset must be conserved in present value, inflation
// and unknown assets are rejected, and with no registry a non-host input is unknown.

#include <coins.h>
#include <compressor.h>
#include <consensus/amount.h>
#include <consensus/asset.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <chainparams.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
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

BOOST_AUTO_TEST_CASE(asset_issuance)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    CCoinsView base;
    CCoinsViewCache view(&base);

    // A definition for a shift=18 asset; the tag is Hash160(def).
    std::vector<unsigned char> def(Consensus::ASSET_DEF_SIZE, 0);
    def[0] = 18;
    const uint160 tag = Consensus::AssetIdFromDef(def);

    std::vector<unsigned char> payload(std::begin(Consensus::ASSET_DEF_MAGIC), std::end(Consensus::ASSET_DEF_MAGIC));
    payload.insert(payload.end(), def.begin(), def.end());
    const CScript opret = CScript() << OP_RETURN << payload;

    // an FRC coin to pay the fee; lock_height == refheight so nothing melts
    const uint32_t H = 1000;
    const COutPoint opHost(Txid::FromUint256(m_rng.rand256()), 0);
    view.AddCoin(opHost, Coin(CTxOut(1000000, CScript() << OP_TRUE), H, 1, false), false);

    // definition tx: OP_RETURN def + mint 100 units + FRC change (2000-kria fee)
    CMutableTransaction mtx;
    mtx.version = 3;
    mtx.lock_height = H;
    mtx.vin.emplace_back(opHost);
    CTxOut mint(10000000000, CScript() << OP_TRUE); mint.assetTag = tag;
    CTxOut marker(0, opret);                                   // definition marker (host, value 0)
    CTxOut change(998000, CScript() << OP_TRUE);               // FRC change
    mtx.vout = {mint, marker, change};
    const CTransaction tx(mtx);

    const auto parsed = Consensus::ParseAssetDefinition(tx);
    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK(parsed->first == tag);
    BOOST_CHECK_EQUAL(parsed->second.shift, 18);
    BOOST_CHECK_EQUAL(parsed->second.granularity, 1U);

    // issuance validates with NO registry: the minted asset is known via the definition and is
    // exempt from the input>=output rule; the host currency still pays the 2000-kria fee.
    TxValidationState state; CAmount fee = -1;
    BOOST_CHECK(Consensus::CheckTxInputs(tx, state, view, consensus, 0, (int)H + 100, Consensus::NONE, fee, nullptr));
    BOOST_CHECK_EQUAL(fee, 2000);
}

BOOST_AUTO_TEST_CASE(asset_tag_utxo_persistence)
{
    std::vector<unsigned char> def(Consensus::ASSET_DEF_SIZE, 0); def[0] = 18;
    const uint160 tag = Consensus::AssetIdFromDef(def);
    CTxOut out(500, CScript() << OP_TRUE);
    out.assetTag = tag;

    // gate OFF (every existing chain): the tag is NOT persisted — a coin round-trips to the host
    // currency, and the serialized bytes are identical to before nV3.
    g_txout_serialize_asset_tag = false;
    {
        DataStream ss;
        ss << Using<TxOutCompression>(out);
        CTxOut back;
        ss >> Using<TxOutCompression>(back);
        BOOST_CHECK(back.assetTag.IsNull());
    }
    // gate ON (the nV3 chain): the tag survives the UTXO serialization round-trip
    g_txout_serialize_asset_tag = true;
    {
        DataStream ss;
        ss << Using<TxOutCompression>(out);
        CTxOut back;
        ss >> Using<TxOutCompression>(back);
        BOOST_CHECK(back.assetTag == tag);
    }
    g_txout_serialize_asset_tag = false;   // restore the global for other tests
}

BOOST_AUTO_TEST_CASE(unique_tokens)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    CCoinsView base;
    CCoinsViewCache view(&base);

    std::vector<unsigned char> def(Consensus::ASSET_DEF_SIZE, 0); def[0] = 20;
    const uint160 tag = Consensus::AssetIdFromDef(def);
    Consensus::AssetRegistry reg;
    reg.Define(tag, Consensus::AssetParams{20, false, 1});

    const std::vector<unsigned char> tokenA = {0xde, 0xad, 0xbe, 0xef};
    // input coin holds asset `tag` (value 0) carrying tokenA
    const COutPoint op(Txid::FromUint256(m_rng.rand256()), 0);
    CTxOut in(0, CScript() << OP_TRUE); in.assetTag = tag; in.tokens = {tokenA};
    view.AddCoin(op, Coin(in, 1000, 1, false), false);

    auto tx_with = [&](std::vector<std::vector<unsigned char>> out_tokens, int nout) {
        CMutableTransaction m; m.version = 3; m.lock_height = 1000; m.vin.emplace_back(op);
        for (int i = 0; i < nout; ++i) { CTxOut o(0, CScript() << OP_TRUE); o.assetTag = tag; o.tokens = out_tokens; m.vout.push_back(o); }
        return CTransaction(m);
    };

    // valid: the token is conserved (moved to a fresh output)
    { const CTransaction tx = tx_with({tokenA}, 1); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1100, Consensus::NONE, f, &reg)); }
    // forge: an output token that was never in the inputs is rejected
    { const CTransaction tx = tx_with({{0xca, 0xfe}}, 1); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1100, Consensus::NONE, f, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-token-created"); }
    // duplicate: the same token in two outputs is rejected (uniqueness)
    { const CTransaction tx = tx_with({tokenA}, 2); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1100, Consensus::NONE, f, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-token-duplicate"); }
}

BOOST_AUTO_TEST_CASE(tx_expiry)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    CCoinsView base;
    CCoinsViewCache view(&base);
    const COutPoint op(Txid::FromUint256(m_rng.rand256()), 0);
    view.AddCoin(op, Coin(CTxOut(1000000, CScript() << OP_TRUE), 1000, 1, false), false);

    auto tx_exp = [&](uint32_t expire) {
        CMutableTransaction m; m.version = 3; m.lock_height = 1000; m.nExpireTime = expire;
        m.vin.emplace_back(op); m.vout.emplace_back(998000, CScript() << OP_TRUE);
        return CTransaction(m);
    };
    // before expiry: valid
    { const CTransaction tx = tx_exp(1005); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1004, Consensus::NONE, f, nullptr)); }
    // past expiry: rejected
    { const CTransaction tx = tx_exp(1005); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1006, Consensus::NONE, f, nullptr));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-expired"); }
    // nExpireTime 0 never expires
    { const CTransaction tx = tx_exp(0); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, 999999, Consensus::NONE, f, nullptr)); }
    // round-trips through serialization (v3 carries nExpireTime)
    { const CTransaction tx = tx_exp(4242);
      DataStream ss; ss << TX_WITH_WITNESS(tx);
      CMutableTransaction back; ss >> TX_WITH_WITNESS(back);
      BOOST_CHECK_EQUAL(back.nExpireTime, 4242u); }
}

BOOST_AUTO_TEST_CASE(asset_registry_serialization)
{
    // The registry round-trips through its serialization (the on-disk assets.dat format is
    // a 5-byte magic + exactly this encoding), so definitions survive a node restart.
    std::vector<unsigned char> defA(2 + 8 + 32, 0); defA[0] = 18;
    std::vector<unsigned char> defB(2 + 8 + 32, 0); defB[0] = 22; defB[1] = 1;
    const uint160 tagA = Consensus::AssetIdFromDef(defA);
    const uint160 tagB = Consensus::AssetIdFromDef(defB);

    Consensus::AssetRegistry reg;
    reg.Define(tagA, Consensus::AssetParams{18, false, 1000});
    reg.Define(tagB, Consensus::AssetParams{22, true, 1});

    DataStream ss;
    ss << reg;
    Consensus::AssetRegistry back;
    ss >> back;

    BOOST_CHECK_EQUAL(back.Size(), 2U);
    BOOST_CHECK(back.IsKnown(tagA));
    BOOST_CHECK(back.IsKnown(tagB));
    const auto a = back.Get(tagA);
    BOOST_CHECK_EQUAL(a.shift, 18);
    BOOST_CHECK(!a.interest);
    BOOST_CHECK_EQUAL(a.granularity, 1000U);
    const auto b = back.Get(tagB);
    BOOST_CHECK_EQUAL(b.shift, 22);
    BOOST_CHECK(b.interest);
    BOOST_CHECK_EQUAL(b.granularity, 1U);
    // an empty registry round-trips too
    Consensus::AssetRegistry empty, empty2;
    DataStream ss2;
    ss2 << empty; ss2 >> empty2;
    BOOST_CHECK_EQUAL(empty2.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(sighash_commits_asset_tag)
{
    // A signature over a version==3 tx must bind each output's asset tag and token set, so no
    // third party can swap WHICH asset (or which tokens) an output pays without invalidating the
    // signature. For every other tx version the sighash is unchanged (the tag is not committed).
    std::vector<unsigned char> defA(2 + 8 + 32, 0); defA[0] = 18;
    std::vector<unsigned char> defB(2 + 8 + 32, 0); defB[0] = 19;
    const uint160 tagA = Consensus::AssetIdFromDef(defA);
    const uint160 tagB = Consensus::AssetIdFromDef(defB);

    auto make = [&](int32_t version, const uint160& tag,
                    const std::vector<std::vector<unsigned char>>& toks) {
        CMutableTransaction m;
        m.version = version;
        m.vin.resize(1);
        m.vin[0].prevout = COutPoint(Txid::FromUint256(m_rng.rand256()), 0);
        m.vout.emplace_back(50000, CScript() << OP_TRUE);
        m.vout[0].assetTag = tag;
        m.vout[0].tokens = toks;
        return m;
    };
    const CScript code = CScript() << OP_TRUE;
    auto H = [&](const CMutableTransaction& m) {
        return SignatureHash(code, m, 0, SIGHASH_ALL, 50000, 1000, SigVersion::WITNESS_V0);
    };

    // Fix the random prevout so only the asset tag / tokens vary between the two hashes.
    const COutPoint fixed(Txid::FromUint256(m_rng.rand256()), 0);
    auto at = [&](int32_t v, const uint160& tag,
                  const std::vector<std::vector<unsigned char>>& toks) {
        CMutableTransaction m = make(v, tag, toks); m.vin[0].prevout = fixed; return m;
    };

    // v3: a different asset tag -> a different sighash (tag-swap breaks the signature).
    BOOST_CHECK(H(at(3, tagA, {})) != H(at(3, tagB, {})));
    // v3: a different token set -> a different sighash.
    BOOST_CHECK(H(at(3, tagA, {{1, 2, 3}})) != H(at(3, tagA, {{4, 5, 6}})));
    // Legacy (BASE) sighash of a v3 tx commits to the tag too.
    auto Hbase = [&](const CMutableTransaction& m) {
        return SignatureHash(code, m, 0, SIGHASH_ALL, 50000, 1000, SigVersion::BASE);
    };
    BOOST_CHECK(Hbase(at(3, tagA, {})) != Hbase(at(3, tagB, {})));
    // Non-v3: the asset tag is NOT part of the sighash — existing sighashes are byte-identical.
    BOOST_CHECK(H(at(1, tagA, {})) == H(at(1, tagB, {})));
    BOOST_CHECK(H(at(2, tagA, {})) == H(at(2, tagB, {})));
}

BOOST_AUTO_TEST_SUITE_END()

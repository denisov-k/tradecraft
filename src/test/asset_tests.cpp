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
#include <hash.h>
#include <key.h>
#include <pubkey.h>
#include <util/strencodings.h>
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

BOOST_AUTO_TEST_CASE(interest_assets)
{
    // Golden vectors generated by the reference model (assets.mjs interestPV) — the C++
    // kernel must match bit-for-bit, including the MAX_MONEY saturation edges.
    struct { CAmount v; uint32_t d; unsigned k; CAmount want; } G[] = {
        {100000000LL, 1, 18, 100000381LL},
        {100000000LL, 1000, 18, 100382197LL},
        {100000000LL, 1000000, 18, 4536269130LL},          // ~x45 over a million blocks
        {1LL, 1, 1, 1LL},                                  // truncation: 1.5 kria -> 1
        {1LL, 100, 1, 9007199254740991LL},                 // factor 1.5^100 -> saturated
        {123456789LL, 52560, 24, 123844163LL},
        {9007199254740991LL, 1, 30, 9007199254740991LL},   // already at MAX_MONEY: clamped
        {5000000000LL, 262144, 20, 6420126318LL},
        {1000000000LL, 33554432, 18, 9007199254740991LL},  // 2^25 blocks -> saturated
        {7LL, 1000000, 8, 9007199254740991LL},
    };
    for (const auto& g : G) {
        BOOST_CHECK_EQUAL(TimeAdjustValueForwardInterestK(g.v, g.d, g.k), g.want);
    }
    // basics: distance 0 and zero value are identities; sign is preserved
    BOOST_CHECK_EQUAL(TimeAdjustValueForwardInterestK(12345, 0, 18), 12345);
    BOOST_CHECK_EQUAL(TimeAdjustValueForwardInterestK(0, 100000, 18), 0);
    BOOST_CHECK_EQUAL(TimeAdjustValueForwardInterestK(-100000000LL, 1000, 18), -100382197LL);

    // CheckTxInputs end-to-end: a bond input may fund MORE nominal out than went in — up to
    // its grown present value and not a kria more.
    const Consensus::Params& consensus = Params().GetConsensus();
    std::vector<unsigned char> def(2 + 8 + 32, 0);
    def[0] = 18; def[1] = 1;   // shift=18, interest flag
    const uint160 bond = Consensus::AssetIdFromDef(def);
    Consensus::AssetRegistry reg;
    reg.Define(bond, Consensus::AssetParams{18, true, 1});

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
    const COutPoint opHost = add(uint160{});
    const COutPoint opBond = add(bond);
    const CAmount grown = TimeAdjustValueForwardInterestK(amt, 1000, 18);   // pv at height 2000
    BOOST_CHECK(grown > amt);

    auto tx_with_bond_out = [&](CAmount bond_out) {
        CMutableTransaction m;
        m.version = 3;
        m.lock_height = 2000;
        m.vin.emplace_back(opHost);
        m.vin.emplace_back(opBond);
        m.vout.emplace_back(1, CScript() << OP_TRUE);   // token host output; rest is fee
        m.vout.emplace_back(bond_out, CScript() << OP_TRUE);
        m.vout.back().assetTag = bond;
        return CTransaction(m);
    };
    { const CTransaction tx = tx_with_bond_out(grown); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, 2000, Consensus::NONE, fee, &reg)); }
    { const CTransaction tx = tx_with_bond_out(grown + 1); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 2000, Consensus::NONE, fee, &reg)); }
    { const CTransaction tx = tx_with_bond_out(grown - 1); TxValidationState st; CAmount fee = 0;
      // non-host assets must conserve EXACTLY — undershooting is as invalid as inflating
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 2000, Consensus::NONE, fee, &reg)); }

    // an out-of-range shift is not a definition at all (kernel domain is 1..64)
    std::vector<unsigned char> bad(2 + 8 + 32, 0);
    bad[0] = 200;
    CMutableTransaction dm;
    dm.version = 3;
    std::vector<unsigned char> payload;
    payload.insert(payload.end(), std::begin(Consensus::ASSET_DEF_MAGIC), std::end(Consensus::ASSET_DEF_MAGIC));
    payload.insert(payload.end(), bad.begin(), bad.end());
    dm.vout.emplace_back(0, CScript() << OP_RETURN << payload);
    BOOST_CHECK(!Consensus::ParseAssetDefinition(CTransaction(dm)).has_value());
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

    // Cross-check against the reference model (core/sighash.mjs): the v3 SINGLE|ANYONECANPAY
    // digest — the DEX offer signature — must match bit-for-bit, tag+tokens committed.
    {
        CMutableTransaction m;
        m.version = 3;
        m.nLockTime = 0;
        m.lock_height = 1234;
        m.vin.resize(1);
        m.vin[0].prevout = COutPoint(Txid::FromUint256(uint256{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}), 1);
        m.vin[0].nSequence = 0xffffffff;
        m.vout.emplace_back(5000, CScript() << OP_0 << std::vector<unsigned char>(20, 0x22));
        {
            const std::vector<unsigned char> tag_bytes = ParseHex("61d2187b9154614c2d5e29cef7cbfdd38f5b1156");
            std::copy(tag_bytes.begin(), tag_bytes.end(), m.vout[0].assetTag.begin());
        }
        m.vout[0].tokens = {{0xde, 0xad, 0xbe, 0xef}};
        const CScript script_code = CScript() << OP_DUP << OP_HASH160
            << std::vector<unsigned char>(20, 0x33) << OP_EQUALVERIFY << OP_CHECKSIG;
        const uint256 got = SignatureHash(script_code, m, 0, SIGHASH_SINGLE | SIGHASH_ANYONECANPAY,
                                          7000, 1200, SigVersion::WITNESS_V0);
        BOOST_CHECK_EQUAL(HexStr(got), "09bf227022e208bf0fe210bea0849e99763dd252d7de4ab42573b6d298dc70dc");
        // nExpireTime is committed: a different expiry is a different digest (and matches
        // the model's vector for expire=777) — no one can impose an expiry on a signed tx.
        m.nExpireTime = 777;
        const uint256 got777 = SignatureHash(script_code, m, 0, SIGHASH_SINGLE | SIGHASH_ANYONECANPAY,
                                             7000, 1200, SigVersion::WITNESS_V0);
        BOOST_CHECK_EQUAL(HexStr(got777), "46dae2ef21f0fe7ac2c594941ee5ea7bc0d0c9787325e5b859ceca1cf576e6a1");
    }
}

BOOST_AUTO_TEST_CASE(authorizers)
{
    // The approval digest must match the reference model bit-for-bit:
    // SHA256d("FRAPPROV" || txid (32 bytes, wire order) || tag (20 bytes)).
    // Golden vector generated by nv3chain.mjs approvalDigest().
    {
        const Txid txid = Txid::FromUint256(uint256{"d33e86d40f79656c0000000000000000000000000000000000000000000000aa"});
        uint160 tag;
        const std::vector<unsigned char> tag_bytes = ParseHex("61d2187b9154614c2d5e29cef7cbfdd38f5b1156");
        std::copy(tag_bytes.begin(), tag_bytes.end(), tag.begin());   // natural (internal) order
        HashWriter ss{};
        ss << std::span{Consensus::ASSET_APPROVAL_TAG} << txid << tag;
        BOOST_CHECK_EQUAL(HexStr(ss.GetHash()), "0eeba01bced4ec4515d02f30c83b3641afeadd291227f2754b06e63ced257d6e");
    }

    // End-to-end: an asset whose definition names an authorizer moves only with that
    // authorizer's signature over the approval digest.
    const Consensus::Params& consensus = Params().GetConsensus();
    CKey auth_key;
    auth_key.MakeNewKey(true);
    const CPubKey auth_pub = auth_key.GetPubKey();

    std::vector<unsigned char> def(2 + 8 + 32, 0);
    def[0] = 20; def[1] = 2;   // shift=20, authorizer flag
    def.insert(def.end(), auth_pub.begin(), auth_pub.end());
    // the def parses, carries the authorizer, and the authorizer is committed in the id
    CMutableTransaction dm;
    dm.version = 3;
    std::vector<unsigned char> payload(std::begin(Consensus::ASSET_DEF_MAGIC), std::end(Consensus::ASSET_DEF_MAGIC));
    payload.insert(payload.end(), def.begin(), def.end());
    dm.vout.emplace_back(0, CScript() << OP_RETURN << payload);
    const auto parsed = Consensus::ParseAssetDefinition(CTransaction(dm));
    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK(parsed->second.authorizer == std::vector<unsigned char>(auth_pub.begin(), auth_pub.end()));
    std::vector<unsigned char> bare(def.begin(), def.begin() + Consensus::ASSET_DEF_SIZE);
    BOOST_CHECK(parsed->first != Consensus::AssetIdFromDef(bare));   // id commits the authorizer
    // flag set but pubkey missing (or vice versa) is not a definition
    {
        CMutableTransaction bad = dm;
        std::vector<unsigned char> p2(std::begin(Consensus::ASSET_DEF_MAGIC), std::end(Consensus::ASSET_DEF_MAGIC));
        p2.insert(p2.end(), def.begin(), def.begin() + Consensus::ASSET_DEF_SIZE);   // flag 2, no key
        bad.vout[0] = CTxOut(0, CScript() << OP_RETURN << p2);
        BOOST_CHECK(!Consensus::ParseAssetDefinition(CTransaction(bad)).has_value());
    }

    const uint160 stock = parsed->first;
    Consensus::AssetRegistry reg;
    reg.Define(stock, parsed->second);

    CCoinsView base;
    CCoinsViewCache view(&base);
    const uint32_t refheight = 1000;
    auto add = [&](const uint160& tag, CAmount amt) {
        const COutPoint op(Txid::FromUint256(m_rng.rand256()), 0);
        CTxOut out(amt, CScript() << OP_TRUE);
        out.assetTag = tag;
        view.AddCoin(op, Coin(out, refheight, 1, false), false);
        return op;
    };
    const COutPoint opHost = add(uint160{}, 100000000);
    const COutPoint opStock = add(stock, 1000);

    auto make_tx = [&](bool approve, const CKey& key) {
        CMutableTransaction m;
        m.version = 3;
        m.lock_height = refheight;   // distance 0: conserve nominal exactly
        m.vin.emplace_back(opHost);
        m.vin.emplace_back(opStock);
        m.vout.emplace_back(1, CScript() << OP_TRUE);
        m.vout.emplace_back(1000, CScript() << OP_TRUE);
        m.vout.back().assetTag = stock;
        if (approve) {
            const Txid txid = CTransaction(m).GetHash();   // approvals don't affect the txid
            HashWriter ss{};
            ss << std::span{Consensus::ASSET_APPROVAL_TAG} << txid << stock;
            std::vector<unsigned char> sig;
            BOOST_REQUIRE(key.Sign(ss.GetHash(), sig));
            m.approvals.emplace_back(stock, sig);
        }
        return CTransaction(m);
    };

    { const CTransaction tx = make_tx(false, auth_key); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::NONE, fee, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-asset-not-authorized"); }
    { const CTransaction tx = make_tx(true, auth_key); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::NONE, fee, &reg)); }
    { CKey wrong; wrong.MakeNewKey(true);
      const CTransaction tx = make_tx(true, wrong); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::NONE, fee, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-asset-authorization-invalid"); }

    // approvals are witness-side: same txid with and without, round-trips through serialization
    { const CTransaction with = make_tx(true, auth_key);
      const CTransaction without = make_tx(false, auth_key);
      BOOST_CHECK(with.GetHash() == without.GetHash());
      BOOST_CHECK(with.GetWitnessHash() != without.GetWitnessHash());
      DataStream ds;
      ds << TX_WITH_WITNESS(with);
      CMutableTransaction back;
      ds >> TX_WITH_WITNESS(back);
      BOOST_CHECK(back.approvals == with.approvals); }
}

BOOST_AUTO_TEST_CASE(dex_bundles)
{
    // -- serialization: the partition rides witness-side (txid unchanged, wtxid distinct) --
    CMutableTransaction m;
    m.version = 3;
    m.lock_height = 1234;
    m.vin.resize(2);
    m.vin[0].prevout = COutPoint(Txid::FromUint256(uint256{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}), 1);
    m.vin[0].nSequence = 0xffffffff;
    m.vin[1].prevout = COutPoint(Txid::FromUint256(uint256{"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}), 0);
    m.vin[1].nSequence = 0xfffffffd;
    m.vout.emplace_back(5000, CScript() << OP_0 << std::vector<unsigned char>(20, 0x22));
    {
        const std::vector<unsigned char> tag_bytes = ParseHex("61d2187b9154614c2d5e29cef7cbfdd38f5b1156");
        std::copy(tag_bytes.begin(), tag_bytes.end(), m.vout[0].assetTag.begin());
    }
    m.vout.emplace_back(700, CScript() << OP_0 << std::vector<unsigned char>(20, 0x44));
    m.bundles.push_back(CBundle{2, 2, 1300});

    { const CTransaction with{m};
      CMutableTransaction bare = m; bare.bundles.clear();
      const CTransaction without{bare};
      BOOST_CHECK(with.GetHash() == without.GetHash());
      BOOST_CHECK(with.GetWitnessHash() != without.GetWitnessHash());
      DataStream ds; ds << TX_WITH_WITNESS(with);
      CMutableTransaction back; ds >> TX_WITH_WITNESS(back);
      BOOST_CHECK(back.bundles == m.bundles); }

    // -- SIGHASH_BUNDLE digests: bit-for-bit against the model (core/sighash.mjs bundleSighash) --
    const CScript code0 = CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 0x33) << OP_EQUALVERIFY << OP_CHECKSIG;
    const CScript code1 = CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 0x55) << OP_EQUALVERIFY << OP_CHECKSIG;
    const int HT = SIGHASH_ALL | SIGHASH_BUNDLE;
    BOOST_CHECK_EQUAL(HexStr(SignatureHash(code0, m, 0, HT, 7000, 1200, SigVersion::WITNESS_V0)),
                      "a297ea1533725a872a03b358c3ccee681ebd2ef5aea8249e7eded87b8d02876f");
    BOOST_CHECK_EQUAL(HexStr(SignatureHash(code1, m, 1, HT, 900, 1100, SigVersion::WITNESS_V0)),
                      "ddcec2104d7c2d018048738567883b41327cab9a5abb262ac327dfe0d234a956");
    { CMutableTransaction m0 = m; m0.bundles[0].nExpireTime = 0;
      BOOST_CHECK_EQUAL(HexStr(SignatureHash(code0, m0, 0, HT, 7000, 1200, SigVersion::WITNESS_V0)),
                        "a34f16db34596188424d933ad8a5074249e932cb7c6430d7708b0c1dc70ddf92"); }

    // -- splice-invariance: graft a matcher leg after the bundle — the digest MUST not move --
    { CMutableTransaction big = m;
      big.vin.resize(3);
      big.vin[2].prevout = COutPoint(Txid::FromUint256(uint256{"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}), 7);
      big.vout.emplace_back(123456, CScript() << OP_0 << std::vector<unsigned char>(20, 0x66));
      BOOST_CHECK_EQUAL(HexStr(SignatureHash(code0, big, 0, HT, 7000, 1200, SigVersion::WITNESS_V0)),
                        "a297ea1533725a872a03b358c3ccee681ebd2ef5aea8249e7eded87b8d02876f");
      // …while tampering INSIDE the bundle moves it
      CMutableTransaction bad = big; bad.vout[1] = CTxOut(701, bad.vout[1].scriptPubKey);
      BOOST_CHECK(SignatureHash(code0, bad, 0, HT, 7000, 1200, SigVersion::WITNESS_V0)
                  != SignatureHash(code0, big, 0, HT, 7000, 1200, SigVersion::WITNESS_V0));
      // an input OUTSIDE every bundle has no bundle digest
      BOOST_CHECK(SignatureHash(code0, big, 2, HT, 1, 1, SigVersion::WITNESS_V0) == uint256::ONE); }

    // -- consensus: per-bundle expiry + partition sanity (flat conservation is unchanged) --
    const Consensus::Params& consensus = Params().GetConsensus();
    CCoinsView base;
    CCoinsViewCache view(&base);
    const uint32_t refheight = 1000;
    auto add = [&](CAmount amt) {
        const COutPoint op(Txid::FromUint256(m_rng.rand256()), 0);
        view.AddCoin(op, Coin(CTxOut(amt, CScript() << OP_TRUE), refheight, 1, false), false);
        return op;
    };
    auto comp = [&](uint32_t expire, uint32_t nIn, uint32_t nOut) {
        CMutableTransaction c;
        c.version = 3;
        c.lock_height = refheight;
        c.vin.emplace_back(add(100000));
        c.vin.emplace_back(add(200000));
        c.vout.emplace_back(90000, CScript() << OP_TRUE);   // bundle: 1 in, 1 out
        c.vout.emplace_back(150000, CScript() << OP_TRUE);  // matcher change; rest = fee
        c.bundles.push_back(CBundle{nIn, nOut, expire});
        return CTransaction(c);
    };
    { const CTransaction tx = comp(0, 1, 1); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1500, Consensus::NONE, fee, nullptr)); }
    { const CTransaction tx = comp(1400, 1, 1); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1500, Consensus::NONE, fee, nullptr));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-bundle-expired"); }
    { const CTransaction tx = comp(0, 3, 1); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1500, Consensus::NONE, fee, nullptr));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-bundle-partition"); }
    { const CTransaction tx = comp(0, 0, 1); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1500, Consensus::NONE, fee, nullptr));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-bundle-empty"); }
}

BOOST_AUTO_TEST_SUITE_END()

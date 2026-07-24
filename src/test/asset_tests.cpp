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
#include <policy/policy.h>
#include <pubkey.h>
#include <util/strencodings.h>
#include <consensus/amount.h>
#include <consensus/asset.h>
#include <consensus/harberger.h>
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

// nVersion=3 EXTENSION-OUTPUT test helpers. The asset tag rides INSIDE the scriptPubKey now, so
// tests build a witness program with the tag in the §XI extension SUFFIX and derive it, exactly as
// the consensus path does at (de)serialization. `AssetScript` = OP_0 <20-byte commitment>
// [<20-byte tag push> OP_1]; the trailing OP_1 is the mandatory extended-output version (v1 =
// fungible). Host currency (null tag) = the plain program. `SetAsset` applies it to a CTxOut.
static CScript AssetScript(const uint160& tag)
{
    CScript s = CScript() << OP_0 << std::vector<unsigned char>(20, 0x11);
    if (!tag.IsNull()) s = s << std::vector<unsigned char>(tag.begin(), tag.end()) << OP_1;
    return s;
}
static void SetAsset(CTxOut& out, const uint160& tag)
{
    out.scriptPubKey = AssetScript(tag);
    out.DeriveAssetTag();
}

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
        CTxOut out(amt, CScript());
        SetAsset(out, tag);
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
        mtx.version = NV3_TX_VERSION;
        mtx.lock_height = H;
        mtx.vin.emplace_back(opHost);
        mtx.vin.emplace_back(opCoop);
        CTxOut o0(coopOut, CScript()); SetAsset(o0, coopTag);
        CTxOut o1(hostOut, AssetScript(uint160{}));   // host currency (null tag)
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
    mtx.version = NV3_TX_VERSION;
    mtx.lock_height = H;
    mtx.vin.emplace_back(opHost);
    CTxOut mint(10000000000, CScript()); SetAsset(mint, tag);
    CTxOut marker(0, opret);                                   // definition marker (host, value 0)
    CTxOut change(998000, AssetScript(uint160{}));             // FRC change
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

// A published definition's bytes are PUBLIC (they sit in the issuance tx's OP_RETURN), so a
// re-publication must not mint more of the asset — and a definition that mints nothing is
// registry spam. Both rejected; mirrors nv3chain.mjs.
BOOST_AUTO_TEST_CASE(asset_redefinition)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    CCoinsView base;
    CCoinsViewCache view(&base);

    std::vector<unsigned char> def(Consensus::ASSET_DEF_SIZE, 0);
    def[0] = 18;
    const uint160 tag = Consensus::AssetIdFromDef(def);

    std::vector<unsigned char> payload(std::begin(Consensus::ASSET_DEF_MAGIC), std::end(Consensus::ASSET_DEF_MAGIC));
    payload.insert(payload.end(), def.begin(), def.end());
    const CScript opret = CScript() << OP_RETURN << payload;

    const uint32_t H = 1000;
    const COutPoint opHost(Txid::FromUint256(m_rng.rand256()), 0);
    view.AddCoin(opHost, Coin(CTxOut(1000000, CScript() << OP_TRUE), H, 1, false), false);

    CMutableTransaction mtx;
    mtx.version = NV3_TX_VERSION;
    mtx.lock_height = H;
    mtx.vin.emplace_back(opHost);
    CTxOut mint(10000000000, CScript()); SetAsset(mint, tag);
    CTxOut marker(0, opret);
    CTxOut change(998000, AssetScript(uint160{}));
    mtx.vout = {mint, marker, change};
    const CTransaction tx(mtx);

    // fresh id: issuance passes
    { Consensus::AssetRegistry reg; TxValidationState st; CAmount fee = -1;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, (int)H + 100, Consensus::NONE, fee, &reg)); }
    // id already in the registry: the identical re-publication is rejected
    { Consensus::AssetRegistry reg; reg.Define(tag, Consensus::AssetParams{18, false, 1});
      TxValidationState st; CAmount fee = -1;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, (int)H + 100, Consensus::NONE, fee, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-asset-redefined"); }
    // a definition with no output of the new asset mints nothing: rejected
    { CMutableTransaction m2 = mtx;
      m2.vout = {marker, change};
      const CTransaction tx2(m2);
      TxValidationState st; CAmount fee = -1;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx2, st, view, consensus, 0, (int)H + 100, Consensus::NONE, fee, nullptr));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-asset-mints-nothing"); }
}

BOOST_AUTO_TEST_CASE(asset_tag_utxo_persistence)
{
    std::vector<unsigned char> def(Consensus::ASSET_DEF_SIZE, 0); def[0] = 18;
    const uint160 tag = Consensus::AssetIdFromDef(def);
    // nVersion=3 EXTENSION-OUTPUT: the tag is NOT a persisted field — it lives inside the
    // scriptPubKey (which chainstate always stores) and is re-derived on load. A full COIN
    // round-trip (Coin::Unserialize calls DeriveAssetTag) must therefore recover the tag on
    // any chain, WITHOUT persisting a separate tag — proving no chainstate-format change for it.
    CTxOut out(500, CScript());
    SetAsset(out, tag);   // ext-push scriptPubKey carrying the tag

    // The bare TxOutCompression (no DeriveAssetTag) preserves the scriptPubKey but leaves the
    // derived field null — confirming the tag is not a separate persisted quantity.
    g_txout_serialize_asset_tag = false;
    {
        DataStream ss;
        ss << Using<TxOutCompression>(out);
        CTxOut back;
        ss >> Using<TxOutCompression>(back);
        BOOST_CHECK(back.assetTag.IsNull());               // not persisted separately…
        BOOST_CHECK(back.scriptPubKey == out.scriptPubKey); // …but the scriptPubKey (carrying it) is
        back.DeriveAssetTag();
        BOOST_CHECK(back.assetTag == tag);                 // …so it is recoverable from the script
    }
    // A COIN round-trip derives the tag automatically on load, on any chain (gate independent).
    for (bool gate : {false, true}) {
        g_txout_serialize_asset_tag = gate;
        DataStream ss;
        Coin coin(out, 1000, 1, false);
        ss << coin;
        Coin back;
        ss >> back;
        BOOST_CHECK(back.out.assetTag == tag);
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

    using Toks = std::vector<std::vector<unsigned char>>;
    const std::vector<unsigned char> tokenA = {0xde, 0xad, 0xbe, 0xef};
    const Toks heldA = {tokenA};

    // nVersion=3 EXTENSION-OUTPUT two-sided reveal helpers ----------------------------------------
    // A v2 token output: base program ++ push(tag) ++ push(H(token-set)) ++ OP_2. GetWitnessExtension
    // yields tag(20)++commit(32) so DeriveAssetTag sets both assetTag and the 32-byte tokenCommit.
    auto assetV2 = [](const uint160& tag_, const Toks& tokens) {
        const uint256 commit = Consensus::TokenSetHash(tokens);
        return CScript() << OP_0 << std::vector<unsigned char>(20, 0x11)
                         << std::vector<unsigned char>(tag_.begin(), tag_.end())
                         << std::vector<unsigned char>(commit.begin(), commit.end()) << OP_2;
    };
    // FRT1 payload: magic ++ output section ++ input section (compactSize-framed, mirrors nv3wire.mjs).
    auto encVarint = [](std::vector<unsigned char>& a, uint64_t n) {
        if (n < 0xfd) a.push_back((unsigned char)n);
        else if (n <= 0xffff) { a.push_back(0xfd); a.push_back(n & 0xff); a.push_back((n >> 8) & 0xff); }
        else { a.push_back(0xfe); for (int i = 0; i < 4; ++i) a.push_back((n >> (8 * i)) & 0xff); }
    };
    auto encSection = [&](std::vector<unsigned char>& a, const std::map<uint32_t, Toks>& m) {
        encVarint(a, m.size());
        for (const auto& [idx, toks] : m) {
            encVarint(a, idx); encVarint(a, toks.size());
            for (const auto& t : toks) { encVarint(a, t.size()); a.insert(a.end(), t.begin(), t.end()); }
        }
    };
    auto reveal = [&](const std::map<uint32_t, Toks>& outR, const std::map<uint32_t, Toks>& inR) {
        std::vector<unsigned char> a(std::begin(Consensus::TOKEN_REVEAL_MAGIC), std::end(Consensus::TOKEN_REVEAL_MAGIC));
        encSection(a, outR); encSection(a, inR);
        return CScript() << OP_RETURN << a;
    };

    // input coin holds asset `tag` (value 0) COMMITTING tokenA — the chainstate keeps only the hash.
    const COutPoint op(Txid::FromUint256(m_rng.rand256()), 0);
    CTxOut in(0, assetV2(tag, heldA)); in.DeriveAssetTag();
    BOOST_CHECK(!in.tokenCommit.IsNull() && in.tokenCommit == Consensus::TokenSetHash(heldA));
    view.AddCoin(op, Coin(in, 1000, 1, false), false);

    // Build a spend of that coin: `nout` committed outputs of `out_tokens`, an FRT1 reveal whose
    // sections are chosen by the flags (to exercise the missing/mismatched-reveal negatives).
    auto tx_with = [&](const Toks& out_tokens, int nout, const std::map<uint32_t, Toks>& outR,
                       const std::map<uint32_t, Toks>& inR) {
        CMutableTransaction m; m.version = 2; m.lock_height = 1000; m.vin.emplace_back(op);
        for (int i = 0; i < nout; ++i) { CTxOut o(0, assetV2(tag, out_tokens)); o.DeriveAssetTag(); m.vout.push_back(o); }
        m.vout.emplace_back(0, reveal(outR, inR));
        return CTransaction(m);
    };
    const std::map<uint32_t, Toks> in0 = {{0, heldA}};   // reveal input 0 = tokenA

    // valid: the token is conserved (moved to a fresh output), both halves revealed
    { const CTransaction tx = tx_with(heldA, 1, {{0, heldA}}, in0); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1100, Consensus::NONE, f, &reg)); }
    // forge: an output token that was never in the inputs is rejected
    { const Toks cafe = {{0xca, 0xfe}};
      const CTransaction tx = tx_with(cafe, 1, {{0, cafe}}, in0); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1100, Consensus::NONE, f, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-token-created"); }
    // duplicate: the same token in two outputs is rejected (uniqueness)
    { const CTransaction tx = tx_with(heldA, 2, {{0, heldA}, {1, heldA}}, in0); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1100, Consensus::NONE, f, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-token-duplicate"); }
    // input commitment WITHOUT reveal → rejected
    { const CTransaction tx = tx_with(heldA, 1, {{0, heldA}}, {}); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1100, Consensus::NONE, f, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-token-input-unrevealed"); }
    // output commitment WITHOUT reveal → rejected
    { const CTransaction tx = tx_with(heldA, 1, {}, in0); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1100, Consensus::NONE, f, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-token-output-unrevealed"); }
    // input reveal that DOESN'T match the coin's commitment → rejected
    { const Toks wrong = {{0x99}};
      const CTransaction tx = tx_with(heldA, 1, {{0, heldA}}, {{0, wrong}}); TxValidationState st; CAmount f = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, 1100, Consensus::NONE, f, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-token-input-mismatch"); }
}

BOOST_AUTO_TEST_CASE(tx_expiry)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    CCoinsView base;
    CCoinsViewCache view(&base);
    const COutPoint op(Txid::FromUint256(m_rng.rand256()), 0);
    view.AddCoin(op, Coin(CTxOut(1000000, CScript() << OP_TRUE), 1000, 1, false), false);

    auto tx_exp = [&](uint32_t expire) {
        CMutableTransaction m; m.version = NV3_TX_VERSION; m.lock_height = 1000; m.nExpireTime = expire;
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
        CTxOut out(amt, CScript());
        SetAsset(out, tag);
        view.AddCoin(op, Coin(out, refheight, 1, false), false);
        return op;
    };
    const COutPoint opHost = add(uint160{});
    const COutPoint opBond = add(bond);
    const CAmount grown = TimeAdjustValueForwardInterestK(amt, 1000, 18);   // pv at height 2000
    BOOST_CHECK(grown > amt);

    auto tx_with_bond_out = [&](CAmount bond_out) {
        CMutableTransaction m;
        m.version = NV3_TX_VERSION;
        m.lock_height = 2000;
        m.vin.emplace_back(opHost);
        m.vin.emplace_back(opBond);
        m.vout.emplace_back(1, AssetScript(uint160{}));  // token host output; rest is fee
        m.vout.emplace_back(bond_out, CScript()); SetAsset(m.vout.back(), bond);
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
    dm.version = NV3_TX_VERSION;
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

    // nVersion=3 EXTENSION-OUTPUT: the asset tag now lives INSIDE scriptPubKey, so it is committed
    // by the ordinary (value, scriptPubKey) serialization on EVERY version — a tag-swap is just a
    // scriptPubKey change. Only the TOKEN set still rides a v3-only parallel block. So the
    // nV3-SPECIFIC sighash property to test is: tokens are committed on v3 and NOT on other versions.
    auto make = [&](int32_t version, const uint160& tag,
                    const std::vector<std::vector<unsigned char>>& toks) {
        CMutableTransaction m;
        m.version = version;
        m.vin.resize(1);
        m.vin[0].prevout = COutPoint(Txid::FromUint256(m_rng.rand256()), 0);
        m.vout.emplace_back(50000, AssetScript(tag));   // tag inside the scriptPubKey
        m.vout[0].DeriveAssetTag();
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

    // The tag is in the scriptPubKey → a tag-swap changes the sighash on ANY version (it is now
    // ordinary script commitment, not an nV3 side-channel).
    BOOST_CHECK(H(at(NV3_TX_VERSION, tagA, {})) != H(at(NV3_TX_VERSION, tagB, {})));
    BOOST_CHECK(H(at(1, tagA, {})) != H(at(1, tagB, {})));
    // v3 token-swap changes the sighash (tokens are committed).
    BOOST_CHECK(H(at(NV3_TX_VERSION, tagA, {{1, 2, 3}})) != H(at(NV3_TX_VERSION, tagA, {{4, 5, 6}})));
    // Non-v3 (incl. TRUC v3 = BIP431, a plain standard version): the TOKEN set is NOT committed —
    // with the same scriptPubKey, varying only tokens leaves the sighash byte-identical.
    BOOST_CHECK(H(at(1, tagA, {{1, 2, 3}})) == H(at(1, tagA, {{4, 5, 6}})));
    BOOST_CHECK(H(at(2, tagA, {{1, 2, 3}})) == H(at(2, tagA, {{4, 5, 6}})));
    BOOST_CHECK(H(at(3, tagA, {{1, 2, 3}})) == H(at(3, tagA, {{4, 5, 6}})));

    // Cross-check against the reference model (core/sighash.mjs): the v3 SINGLE|ANYONECANPAY
    // digest — the DEX offer signature — must match bit-for-bit. Tag is in the scriptPubKey §XI
    // suffix (0014{22×20} ++ push(tag) ++ OP_1); the token set is committed via the v3 parallel block.
    {
        CMutableTransaction m;
        m.version = NV3_TX_VERSION;
        m.nLockTime = 0;
        m.lock_height = 1234;
        m.vin.resize(1);
        m.vin[0].prevout = COutPoint(Txid::FromUint256(uint256{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}), 1);
        m.vin[0].nSequence = 0xffffffff;
        {
            const std::vector<unsigned char> tag_bytes = ParseHex("61d2187b9154614c2d5e29cef7cbfdd38f5b1156");
            CScript spk = CScript() << OP_0 << std::vector<unsigned char>(20, 0x22) << tag_bytes << OP_1;
            m.vout.emplace_back(5000, spk);
        }
        m.vout[0].tokens = {{0xde, 0xad, 0xbe, 0xef}};
        const CScript script_code = CScript() << OP_DUP << OP_HASH160
            << std::vector<unsigned char>(20, 0x33) << OP_EQUALVERIFY << OP_CHECKSIG;
        const uint256 got = SignatureHash(script_code, m, 0, SIGHASH_SINGLE | SIGHASH_ANYONECANPAY,
                                          7000, 1200, SigVersion::WITNESS_V0);
        BOOST_CHECK_EQUAL(HexStr(got), "204578f1dd81b214260ed88a47c0841fae29a6518373ef05f9a0207f73581763");
        // nExpireTime is committed: a different expiry is a different digest (and matches
        // the model's vector for expire=777) — no one can impose an expiry on a signed tx.
        m.nExpireTime = 777;
        const uint256 got777 = SignatureHash(script_code, m, 0, SIGHASH_SINGLE | SIGHASH_ANYONECANPAY,
                                             7000, 1200, SigVersion::WITNESS_V0);
        BOOST_CHECK_EQUAL(HexStr(got777), "1ef7ce08ae9d9472ffcbfa39380218a6da4c4111cb94dedf3198909986cdcf41");
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
    dm.version = NV3_TX_VERSION;
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
        CTxOut out(amt, CScript());
        SetAsset(out, tag);
        view.AddCoin(op, Coin(out, refheight, 1, false), false);
        return op;
    };
    const COutPoint opHost = add(uint160{}, 100000000);
    const COutPoint opStock = add(stock, 1000);

    auto make_tx = [&](bool approve, const CKey& key) {
        CMutableTransaction m;
        m.version = NV3_TX_VERSION;
        m.lock_height = refheight;   // distance 0: conserve nominal exactly
        m.vin.emplace_back(opHost);
        m.vin.emplace_back(opStock);
        m.vout.emplace_back(1, CScript() << OP_TRUE);
        m.vout.emplace_back(1000, CScript() << OP_TRUE);
        SetAsset(m.vout.back(), stock);
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
    m.version = NV3_TX_VERSION;
    m.lock_height = 1234;
    m.vin.resize(2);
    m.vin[0].prevout = COutPoint(Txid::FromUint256(uint256{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}), 1);
    m.vin[0].nSequence = 0xffffffff;
    m.vin[1].prevout = COutPoint(Txid::FromUint256(uint256{"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}), 0);
    m.vin[1].nSequence = 0xfffffffd;
    // nVersion=3 EXTENSION-OUTPUT: the tag rides in the scriptPubKey (0022×20 ++ push(tag)).
    m.vout.emplace_back(5000, CScript() << OP_0 << std::vector<unsigned char>(20, 0x22)
                                        << ParseHex("61d2187b9154614c2d5e29cef7cbfdd38f5b1156"));
    m.vout[0].DeriveAssetTag();
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
                      "4f35bee31f42713ded353e71be07d9079eb372c73d0bc3644af3d1780ef013b6");
    BOOST_CHECK_EQUAL(HexStr(SignatureHash(code1, m, 1, HT, 900, 1100, SigVersion::WITNESS_V0)),
                      "adacf313bc018e2920708e242c2d3358d1c4238387ca1d677cf868c39f29244b");
    { CMutableTransaction m0 = m; m0.bundles[0].nExpireTime = 0;
      BOOST_CHECK_EQUAL(HexStr(SignatureHash(code0, m0, 0, HT, 7000, 1200, SigVersion::WITNESS_V0)),
                        "d563923f84bdaf7f294b74bd15193e0b840cebcf6c8af76dd7d9ce660eca78a2"); }

    // -- splice-invariance: graft a matcher leg after the bundle — the digest MUST not move --
    { CMutableTransaction big = m;
      big.vin.resize(3);
      big.vin[2].prevout = COutPoint(Txid::FromUint256(uint256{"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}), 7);
      big.vout.emplace_back(123456, CScript() << OP_0 << std::vector<unsigned char>(20, 0x66));
      BOOST_CHECK_EQUAL(HexStr(SignatureHash(code0, big, 0, HT, 7000, 1200, SigVersion::WITNESS_V0)),
                        "4f35bee31f42713ded353e71be07d9079eb372c73d0bc3644af3d1780ef013b6");
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
        c.version = NV3_TX_VERSION;
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

BOOST_AUTO_TEST_CASE(dex_ranged)
{
    // -- digest cross-vectors: the SIGHASH_BUNDLE digest of a RANGED input commits the
    // descriptor (not the outputs); generated by the model (core/sighash.mjs rangedSighash) --
    CMutableTransaction m;
    m.version = NV3_TX_VERSION;
    m.lock_height = 1234;
    m.vin.resize(1);
    m.vin[0].prevout = COutPoint(Txid::FromUint256(uint256{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}), 1);
    m.vin[0].nSequence = 0xffffffff;
    m.vout.emplace_back(21000000, CScript() << OP_0 << std::vector<unsigned char>(20, 0x22));   // payout (miner-chosen)
    m.vout.emplace_back(300, CScript() << OP_0 << std::vector<unsigned char>(20, 0x44));        // change
    CRangedBundle r;
    r.nIn = 1;
    r.payoutAsset = uint160();
    r.payoutScript = CScript() << OP_0 << std::vector<unsigned char>(20, 0x22);
    r.priceNum = 30000; r.priceDen = 1;
    r.changeScript = CScript() << OP_0 << std::vector<unsigned char>(20, 0x44);
    r.minFill = 100; r.maxFill = 800;
    r.nExpireTime = 1300;
    m.ranged.push_back(r);

    const CScript code = CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 0x33) << OP_EQUALVERIFY << OP_CHECKSIG;
    const int HT = SIGHASH_ALL | SIGHASH_BUNDLE;
    BOOST_CHECK_EQUAL(HexStr(SignatureHash(code, m, 0, HT, 7000, 1200, SigVersion::WITNESS_V0)),
                      "cbaf83adeab9277e44c4055b3f93447706afde5ace562988fb1117e867c27071");
    { CMutableTransaction m2 = m; m2.ranged[0].priceNum = 29999;
      BOOST_CHECK_EQUAL(HexStr(SignatureHash(code, m2, 0, HT, 7000, 1200, SigVersion::WITNESS_V0)),
                        "8afda870c437f359cb99ff7f4362aef5ffaf3700e9771479762076f5cf9d4164"); }
    // the digest does NOT move when the miner changes the fill (outputs) — one signature,
    // any admissible fill
    { CMutableTransaction m3 = m;
      m3.vout[0] = CTxOut(15000000, r.payoutScript);
      m3.vout[1] = CTxOut(500, r.changeScript);
      BOOST_CHECK_EQUAL(HexStr(SignatureHash(code, m3, 0, HT, 7000, 1200, SigVersion::WITNESS_V0)),
                        "cbaf83adeab9277e44c4055b3f93447706afde5ace562988fb1117e867c27071"); }

    // -- serialization round-trip (witness-side, txid unchanged) --
    { const CTransaction with{m};
      CMutableTransaction bare = m; bare.ranged.clear();
      BOOST_CHECK(with.GetHash() == CTransaction(bare).GetHash());
      BOOST_CHECK(with.GetWitnessHash() != CTransaction(bare).GetWitnessHash());
      DataStream ds; ds << TX_WITH_WITNESS(with);
      CMutableTransaction back; ds >> TX_WITH_WITNESS(back);
      BOOST_CHECK(back.ranged == m.ranged); }

    // -- consensus: the miner's fill must satisfy the signed constraint --
    const Consensus::Params& consensus = Params().GetConsensus();
    std::vector<unsigned char> def(2 + 8 + 32, 0);
    def[0] = 18;
    const uint160 coop = Consensus::AssetIdFromDef(def);
    Consensus::AssetRegistry reg;
    reg.Define(coop, Consensus::AssetParams{18, false, 1});

    CCoinsView base;
    CCoinsViewCache view(&base);
    const uint32_t refheight = 1000;
    auto add = [&](const uint160& tag, CAmount amt) {
        const COutPoint op(Txid::FromUint256(m_rng.rand256()), 0);
        CTxOut out(amt, CScript());
        SetAsset(out, tag);
        view.AddCoin(op, Coin(out, refheight, 1, false), false);
        return op;
    };
    const COutPoint opCoop = add(coop, 1000);         // the ranged give (distance 0: pv = 1000)
    const COutPoint opFrc = add(uint160{}, 50000000); // taker's FRC

    auto comp = [&](CAmount pay_val, CAmount change_val, CAmount maxFill) {
        CMutableTransaction c;
        c.version = NV3_TX_VERSION;
        c.lock_height = refheight;
        c.vin.emplace_back(opCoop);
        c.vin.emplace_back(opFrc);
        // nVersion=3 EXTENSION-OUTPUT: an asset-bearing output is a witness program with the tag
        // in the extension push; the descriptor references the BASE program (tag stripped). The
        // host payout can stay a bare script (no asset ⇒ no ext push).
        const CScript changeBase = CScript() << OP_0 << std::vector<unsigned char>(20, 0x77);
        CRangedBundle rb;
        rb.nIn = 1;
        rb.payoutAsset = uint160();
        rb.payoutScript = CScript() << OP_1;
        rb.priceNum = 30000; rb.priceDen = 1;
        rb.changeScript = changeBase;                                    // base (tag stripped)
        rb.minFill = 100; rb.maxFill = maxFill;
        c.ranged.push_back(rb);
        c.vout.emplace_back(pay_val, CScript() << OP_1);                 // payout (host)
        CScript changeSpk = changeBase; changeSpk << std::vector<unsigned char>(coop.begin(), coop.end()) << OP_1;
        c.vout.emplace_back(change_val, changeSpk);                      // change (coop, tag in §XI suffix + v1 opcode)
        c.vout.back().DeriveAssetTag();
        c.vout.emplace_back(1000000, CScript() << OP_TRUE);              // taker keeps FRC change; coop→?
        // taker's coop receipt: fill kria of coop
        c.vout.emplace_back(1000 - change_val, CScript() << OP_TRUE);
        SetAsset(c.vout.back(), coop);
        return CTransaction(c);
    };
    // fill = 700 at price 30000 -> payout 21e6: valid
    { const CTransaction tx = comp(21000000, 300, 800); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::NONE, fee, &reg)); }
    // shaving the payout below the price: rejected
    { const CTransaction tx = comp(20999999, 300, 800); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::NONE, fee, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-ranged-price"); }
    // overfill past the signed bound: rejected
    { const CTransaction tx = comp(21000000, 300, 600); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::NONE, fee, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-ranged-fill-bounds"); }
    // stealing the change destination (a different base program): rejected
    { CMutableTransaction c{comp(21000000, 300, 800)};
      CScript stolen = CScript() << OP_0 << std::vector<unsigned char>(20, 0x99);
      stolen << std::vector<unsigned char>(coop.begin(), coop.end()) << OP_1;
      c.vout[1] = CTxOut(300, stolen);
      c.vout[1].DeriveAssetTag();
      const CTransaction tx{c}; TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::NONE, fee, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-ranged-destination"); }
}

// Freiland Harberger covenant (docs/freiland-covenant-spec.md §3): the OP_3 witness-v2 output
// format, decoded by Consensus::ParseHarbergerOutput. Rule-free step — parsing only, mirroring the
// JS model (core/asset-spk.mjs / apps/web/test/harberger.test.mjs). No validation path yet.
static CScript BuildHarberger(const uint256& name, const uint160& owner, CAmount floorV)
{
    std::vector<unsigned char> fv(8);
    for (int i = 0; i < 8; ++i) { fv[i] = (unsigned char)(floorV & 0xff); floorV >>= 8; }
    return CScript() << OP_1
        << std::vector<unsigned char>(name.begin(), name.end())
        << std::vector<unsigned char>(owner.begin(), owner.end())
        << fv << OP_3;
}

BOOST_AUTO_TEST_CASE(harberger_output_format)
{
    uint256 name; for (int i = 0; i < 32; ++i) name.begin()[i] = 0xaa;
    uint160 owner; for (int i = 0; i < 20; ++i) owner.begin()[i] = 0xbb;
    const CAmount floorV = 1000000; // 0.01 FRC dust floor

    const CScript spk = BuildHarberger(name, owner, floorV);
    // exact wire form: OP_1 20{name} 14{owner} 08{floorV LE} OP_3  ⇒ 65 bytes
    BOOST_CHECK_EQUAL(spk.size(), Consensus::HARBERGER_SPK_SIZE);
    BOOST_CHECK_EQUAL(spk[0], OP_1);   // witness version 2 (anyone-can-spend on old nodes)
    BOOST_CHECK_EQUAL(spk[64], OP_3);  // HRBG extended-output marker

    Consensus::HarbergerCovenant h;
    BOOST_CHECK(Consensus::ParseHarbergerOutput(spk, h));
    BOOST_CHECK(h.nameHash == name);
    BOOST_CHECK(h.owner == owner);
    BOOST_CHECK_EQUAL(h.floorV, floorV);
    BOOST_CHECK(Consensus::IsHarbergerOutput(spk));

    // floorV little-endian round-trips across the range
    for (CAmount v : {CAmount{0}, CAmount{1}, CAmount{255}, CAmount{256}, CAmount{100000000}, CAmount{0xdeadbeef}, MAX_MONEY}) {
        Consensus::HarbergerCovenant r;
        BOOST_CHECK(Consensus::ParseHarbergerOutput(BuildHarberger(name, owner, v), r));
        BOOST_CHECK_EQUAL(r.floorV, v);
    }

    // NOT a HRBG output: a host program, a fungible asset (OP_1 suffix), an asset+tokens (OP_2), and
    // a HRBG-shaped script with the wrong trailing marker must all be rejected.
    Consensus::HarbergerCovenant j;
    BOOST_CHECK(!Consensus::ParseHarbergerOutput(CScript() << OP_0 << std::vector<unsigned char>(20, 0x11), j));
    BOOST_CHECK(!Consensus::ParseHarbergerOutput(AssetScript(uint160{std::vector<unsigned char>(20, 0xcc)}), j));
    CScript wrong = CScript() << OP_1
        << std::vector<unsigned char>(name.begin(), name.end())
        << std::vector<unsigned char>(owner.begin(), owner.end())
        << std::vector<unsigned char>(8, 0) << OP_2;   // OP_2, not OP_3
    BOOST_CHECK(!Consensus::ParseHarbergerOutput(wrong, j));
}

// Path-A forced buy at the CheckTxInputs level (docs/freiland-covenant-spec.md §4). When HARBERGER
// is active, spending a HRBG deposit requires paying V=asset_pv(deposit) to the owner AND a
// successor HRBG output for the same name with value>=V. Without the flag the covenant is inert.
BOOST_AUTO_TEST_CASE(harberger_forced_buy)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    Consensus::AssetRegistry reg;
    CCoinsView base; CCoinsViewCache view(&base);
    const uint32_t refheight = 1000;

    uint256 name; for (int i = 0; i < 32; ++i) name.begin()[i] = 0xaa;
    uint160 alice; for (int i = 0; i < 20; ++i) alice.begin()[i] = 0xbb;   // current owner (paid V)
    uint160 bob;   for (int i = 0; i < 20; ++i) bob.begin()[i]   = 0xcc;   // buyer's new owner
    const CAmount V = 1000;                                                 // present value at distance 0

    auto add = [&](const CScript& spk, CAmount amt) {
        const COutPoint op(Txid::FromUint256(m_rng.rand256()), 0);
        CTxOut out(amt, spk); out.DeriveAssetTag();
        view.AddCoin(op, Coin(out, refheight, 1, false), false);
        return op;
    };
    const COutPoint opHrbg = add(BuildHarberger(name, alice, 100 /*floorV*/), V);   // host FRC deposit
    const COutPoint opFrc  = add(CScript() << OP_TRUE, 50000);                       // buyer's funding
    const CScript payAlice = CScript() << OP_0 << std::vector<unsigned char>(alice.begin(), alice.end());

    auto buildBuy = [&](CAmount payVal, bool successor, CAmount succVal) {
        CMutableTransaction c; c.version = NV3_TX_VERSION; c.lock_height = refheight;
        c.vin.emplace_back(opHrbg); c.vin.emplace_back(opFrc);
        c.vout.emplace_back(payVal, payAlice);                                          // (1) pay owner
        if (successor) { CTxOut s(succVal, BuildHarberger(name, bob, 100)); s.DeriveAssetTag(); c.vout.push_back(s); } // (2) successor
        c.vout.emplace_back(500, CScript() << OP_TRUE);                                 // slack → fee
        return CTransaction(c);
    };

    // valid forced buy
    { auto tx = buildBuy(V, true, V); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::HARBERGER, fee, &reg)); }
    // underpaying the owner
    { auto tx = buildBuy(V - 1, true, V); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::HARBERGER, fee, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-harberger-unpaid"); }
    // no successor (would let a buyer take the name and pocket the deposit)
    { auto tx = buildBuy(V, false, V); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::HARBERGER, fee, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-harberger-no-successor"); }
    // successor deposit below V (buyer under-funds the carried deposit)
    { auto tx = buildBuy(V, true, V - 1); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::HARBERGER, fee, &reg));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-harberger-no-successor"); }
    // WITHOUT the flag: the HRBG deposit is an ordinary (anyone-can-spend) host coin — spending it
    // with no owner payout and no successor is valid; the covenant is not enforced.
    { CMutableTransaction c; c.version = NV3_TX_VERSION; c.lock_height = refheight;
      c.vin.emplace_back(opHrbg); c.vin.emplace_back(opFrc);
      c.vout.emplace_back(V + 40000, CScript() << OP_TRUE);
      CTransaction tx(c); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::NONE, fee, &reg)); }
}

// Name UNIQUENESS (docs/freiland-covenant-spec.md §4): at most one live HRBG output per name.
BOOST_AUTO_TEST_CASE(harberger_uniqueness)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    Consensus::AssetRegistry reg;
    CCoinsView base; CCoinsViewCache view(&base);
    const uint32_t refheight = 1000;

    uint256 N; for (int i = 0; i < 32; ++i) N.begin()[i] = 0xaa;      // an already-claimed name
    uint256 M; for (int i = 0; i < 32; ++i) M.begin()[i] = 0xdd;      // a free name
    uint160 alice; for (int i = 0; i < 20; ++i) alice.begin()[i] = 0xbb;
    uint160 bob;   for (int i = 0; i < 20; ++i) bob.begin()[i]   = 0xcc;
    const CAmount V = 1000;

    auto add = [&](const CScript& spk, CAmount amt) {
        const COutPoint op(Txid::FromUint256(m_rng.rand256()), 0);
        CTxOut out(amt, spk); out.DeriveAssetTag();
        view.AddCoin(op, Coin(out, refheight, 1, false), false);
        return op;
    };
    const COutPoint opN   = add(BuildHarberger(N, alice, 100), V);    // N's live holder
    const COutPoint opFrc = add(CScript() << OP_TRUE, 50000);
    const CScript payAlice = CScript() << OP_0 << std::vector<unsigned char>(alice.begin(), alice.end());

    Consensus::NameRegistry names;
    names.Claim(N, opN);                                             // N is live at opN

    // fresh claim of a FREE name M (no HRBG input) — allowed
    { CMutableTransaction c; c.version = NV3_TX_VERSION; c.lock_height = refheight;
      c.vin.emplace_back(opFrc);
      c.vout.emplace_back(V, BuildHarberger(M, bob, 100)); c.vout.back().DeriveAssetTag();
      c.vout.emplace_back(40000, CScript() << OP_TRUE);
      CTransaction tx(c); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::HARBERGER, fee, &reg, &names)); }

    // claim of the TAKEN name N without spending its holder — rejected
    { CMutableTransaction c; c.version = NV3_TX_VERSION; c.lock_height = refheight;
      c.vin.emplace_back(opFrc);
      c.vout.emplace_back(V, BuildHarberger(N, bob, 100)); c.vout.back().DeriveAssetTag();
      c.vout.emplace_back(40000, CScript() << OP_TRUE);
      CTransaction tx(c); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::HARBERGER, fee, &reg, &names));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-harberger-name-taken"); }

    // two HRBG outputs for the same name in one tx — rejected
    { CMutableTransaction c; c.version = NV3_TX_VERSION; c.lock_height = refheight;
      c.vin.emplace_back(opFrc);
      c.vout.emplace_back(V, BuildHarberger(M, bob, 100)); c.vout.back().DeriveAssetTag();
      c.vout.emplace_back(V, BuildHarberger(M, alice, 100)); c.vout.back().DeriveAssetTag();
      CTransaction tx(c); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(!Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::HARBERGER, fee, &reg, &names));
      BOOST_CHECK_EQUAL(st.GetRejectReason(), "bad-txns-harberger-dup-name"); }

    // valid TRANSFER of N: spends N's holder (opN) + forced-buy (pay V to Alice + successor N) — allowed
    { CMutableTransaction c; c.version = NV3_TX_VERSION; c.lock_height = refheight;
      c.vin.emplace_back(opN); c.vin.emplace_back(opFrc);
      c.vout.emplace_back(V, payAlice);
      c.vout.emplace_back(V, BuildHarberger(N, bob, 100)); c.vout.back().DeriveAssetTag();
      c.vout.emplace_back(40000, CScript() << OP_TRUE);
      CTransaction tx(c); TxValidationState st; CAmount fee = 0;
      BOOST_CHECK(Consensus::CheckTxInputs(tx, st, view, consensus, 0, refheight, Consensus::HARBERGER, fee, &reg, &names)); }
}

// RELAY standardness (docs/freiland-covenant-spec.md §Осталось): a HRBG output is an anyone-can-spend
// unknown-witness-version program, so a forced buy that spends it must be relayable — AreInputsStandard
// must NOT reject a HRBG input the way it rejects a generic WITNESS_UNKNOWN input. (The DISCOURAGE
// policy flag is relaxed for HRBG spends in MemPoolAccept::PolicyScriptChecks; that path needs the
// mempool and is covered by the functional tests, not here.)
BOOST_AUTO_TEST_CASE(harberger_relay_standardness)
{
    CCoinsView base; CCoinsViewCache view(&base);

    uint256 name; for (int i = 0; i < 32; ++i) name.begin()[i] = 0xaa;
    uint160 owner; for (int i = 0; i < 20; ++i) owner.begin()[i] = 0xbb;

    auto add = [&](const CScript& spk) {
        const COutPoint op(Txid::FromUint256(m_rng.rand256()), 0);
        CTxOut out(1000, spk); out.DeriveAssetTag();
        view.AddCoin(op, Coin(out, 1000, 1, false), false);
        return op;
    };
    const COutPoint opHrbg = add(BuildHarberger(name, owner, 100));
    // a generic unknown-witness-version program that is NOT a HRBG covenant (no 65-byte OP_3 suffix)
    const COutPoint opUnknown = add(CScript() << OP_1 << std::vector<unsigned char>(32, 0x77));

    auto spend = [&](const COutPoint& op) {
        CMutableTransaction c; c.version = NV3_TX_VERSION; c.lock_height = 1000;
        c.vin.emplace_back(op);
        c.vout.emplace_back(500, CScript() << OP_TRUE);
        return CTransaction(c);
    };

    // spending a HRBG covenant input is relay-standard (exempted), spending a generic unknown
    // witness program is not — proving the exemption is narrow to the HRBG format.
    BOOST_CHECK(::AreInputsStandard(spend(opHrbg), view));
    BOOST_CHECK(!::AreInputsStandard(spend(opUnknown), view));
}

BOOST_AUTO_TEST_SUITE_END()

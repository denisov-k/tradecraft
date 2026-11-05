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

#include <coins.h>
#include <consensus/assets.h>
#include <consensus/confidential.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>

#include <secp256k1.h>
#include <secp256k1_generator.h>
#include <secp256k1_rangeproof.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(confidential_tests, BasicTestingSetup)

namespace {
const std::vector<unsigned char> PROGRAM_A(32, 0x22);
const std::vector<unsigned char> PROGRAM_B(32, 0x33);

struct Blind {
    unsigned char b[32];
};

/** Builds confidential scrip outputs the way a wallet would. */
struct Maker {
    secp256k1_context* ctx{secp256k1_context_create(SECP256K1_CONTEXT_NONE)};
    secp256k1_generator gen;
    Maker() { BOOST_REQUIRE(secp256k1_generator_generate(ctx, &gen, ScripAssetId().begin())); }
    ~Maker() { secp256k1_context_destroy(ctx); }

    Blind Negate(const Blind& in)
    {
        Blind out;
        const unsigned char* blinds[1] = {in.b};
        BOOST_REQUIRE(secp256k1_pedersen_blind_sum(ctx, out.b, blinds, 1, 0));
        return out;
    }

    /** An output of `value` scrip blinded by `blind` on `program`, and its range proof
     *  (made for `proof_program`, the same unless a test says otherwise). */
    std::pair<CTxOut, std::vector<unsigned char>> Make(uint64_t value, const Blind& blind, const std::vector<unsigned char>& program,
                                                      const std::vector<unsigned char>* proof_program = nullptr)
    {
        secp256k1_pedersen_commitment commit;
        BOOST_REQUIRE(secp256k1_pedersen_commit(ctx, &commit, blind.b, value, &gen));
        ConfidentialOutput c;
        BOOST_REQUIRE(secp256k1_pedersen_commitment_serialize(ctx, c.commitment.data(), &commit));
        c.nonce_key[0] = 0x02;
        c.nonce_key[1] = 0x77;
        const CScript base = CScript() << OP_0 << program;
        const CScript bound = CScript() << OP_0 << (proof_program ? *proof_program : program);
        std::vector<unsigned char> proof(MAX_CONFIDENTIAL_PROOF_SIZE);
        size_t plen = proof.size();
        unsigned char nonce[32] = {0x42};
        BOOST_REQUIRE(secp256k1_rangeproof_sign(ctx, proof.data(), &plen, 0, &commit, blind.b, nonce, 0, 0, value,
                                                nullptr, 0, bound.data(), bound.size(), &gen));
        proof.resize(plen);
        return {CTxOut(0, CScript(base) << ConfidentialExtension(c)), proof};
    }
};

struct Fixture {
    CCoinsView base;
    CCoinsViewCache view{&base};
    uint32_t next{1};

    COutPoint Add(const CTxOut& out, int height)
    {
        const COutPoint prevout(Txid::FromUint256(uint256{static_cast<uint8_t>(next++)}), 0);
        view.AddCoin(prevout, Coin(out, /*refheight=*/height, height, /*fCoinBaseIn=*/false), false);
        return prevout;
    }

    bool Check(const std::vector<COutPoint>& in, const std::vector<std::pair<CTxOut, std::vector<unsigned char>>>& out,
               std::string& reason, int confidential_height = 100)
    {
        CMutableTransaction tx;
        for (const COutPoint& prevout : in) tx.vin.emplace_back(prevout);
        tx.output_witness.resize(out.size());
        for (size_t n = 0; n < out.size(); ++n) {
            tx.vout.push_back(out[n].first);
            if (!out[n].second.empty()) tx.output_witness[n].stack = {out[n].second};
        }
        TxValidationState state;
        const bool ok = Consensus::CheckTxAssets(CTransaction(tx), view, /*asset_height=*/100, state, /*block_final=*/false, confidential_height);
        reason = state.GetRejectReason();
        return ok;
    }
};

std::pair<CTxOut, std::vector<unsigned char>> Explicit(CAmount scrip)
{
    return {CTxOut(0, CScript() << OP_0 << PROGRAM_A << AssetExtension({ScripAssetId(), scrip})), {}};
}

const Blind B1{{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x01}};
} // namespace

BOOST_AUTO_TEST_CASE(confidential_outputs)
{
    Maker m;
    Fixture f;
    std::string reason;
    const Blind b2 = m.Negate(B1);
    const COutPoint held = f.Add(Explicit(1000).first, 150);

    // Explicit scrip into two confidential outputs whose blinding factors cancel.
    BOOST_CHECK(f.Check({held}, {m.Make(600, B1, PROGRAM_A), m.Make(400, b2, PROGRAM_B)}, reason));
    // the amounts must balance
    BOOST_CHECK(!f.Check({held}, {m.Make(600, B1, PROGRAM_A), m.Make(401, b2, PROGRAM_B)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-ct-balance");
    // with explicit change too
    BOOST_CHECK(f.Check({held}, {m.Make(600, B1, PROGRAM_A), m.Make(300, b2, PROGRAM_B), Explicit(100)}, reason));

    // Each output needs its range proof, made for its own program.
    auto missing = m.Make(600, B1, PROGRAM_A);
    missing.second.clear();
    BOOST_CHECK(!f.Check({held}, {missing, m.Make(400, b2, PROGRAM_B)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-ct-proof-missing");
    BOOST_CHECK(!f.Check({held}, {m.Make(600, B1, PROGRAM_A, &PROGRAM_B), m.Make(400, b2, PROGRAM_B)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-ct-proof");
    // a proof may not reach beyond MAX_MONEY, even if the sum works out mod the group order
    BOOST_CHECK(!f.Check({held}, {m.Make(uint64_t{1} << 60, B1, PROGRAM_A), m.Make(400, b2, PROGRAM_B)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-ct-range");
    // and holds no freicoin
    auto valued = m.Make(600, B1, PROGRAM_A);
    valued.first.SetReferenceValue(1);
    BOOST_CHECK(!f.Check({held}, {valued, m.Make(400, b2, PROGRAM_B)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-ct-nonzero-value");

    // Spending confidential scrip: the blinding factor carries over.
    const auto c600 = m.Make(600, B1, PROGRAM_A);
    const COutPoint hidden = f.Add(c600.first, 150);
    BOOST_CHECK(f.Check({hidden}, {m.Make(400, B1, PROGRAM_B), Explicit(200)}, reason));
    BOOST_CHECK(!f.Check({hidden}, {m.Make(400, B1, PROGRAM_B), Explicit(201)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-ct-balance");
    // a commitment made before the rules applied counts for nothing
    const COutPoint early = f.Add(c600.first, 50);
    BOOST_CHECK(!f.Check({early}, {m.Make(400, B1, PROGRAM_B), Explicit(200)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-ct-balance");

    // Before the rules, a confidential output is an unknown extension: nothing checks it.
    BOOST_CHECK(f.Check({held}, {missing}, reason, /*confidential_height=*/-1));
}

BOOST_AUTO_TEST_CASE(confidential_placement)
{
    Maker m;
    Fixture f;
    // never in the coinbase
    CMutableTransaction cb;
    cb.vin.emplace_back(COutPoint());
    cb.vin[0].scriptSig = CScript() << 1 << OP_0;
    const auto out = m.Make(600, B1, PROGRAM_A);
    cb.vout.push_back(out.first);
    cb.output_witness.resize(1);
    cb.output_witness[0].stack = {out.second};
    TxValidationState state;
    BOOST_CHECK(!Consensus::CheckTxAssets(CTransaction(cb), f.view, 100, state, false, 100));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-ct-coinbase");
    // the extension parses back
    ConfidentialOutput c;
    BOOST_CHECK(GetConfidentialOutput(out.first, c) == ConfidentialOutputType::CONFIDENTIAL);
    BOOST_CHECK(GetConfidentialOutput(Explicit(5).first, c) == ConfidentialOutputType::NONE);
}

BOOST_AUTO_TEST_SUITE_END()

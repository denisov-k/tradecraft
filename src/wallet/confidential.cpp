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

#include <wallet/confidential.h>

#include <consensus/assets.h>
#include <consensus/confidential.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <random.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <secp256k1_generator.h>
#include <secp256k1_rangeproof.h>

#include <algorithm>
#include <cassert>

namespace wallet {
namespace {
/** Proofs keep 52 bits private, as Elements does: amounts below 2^52 kria of scrip
 *  look alike. */
constexpr int PROOF_MIN_BITS = 52;

secp256k1_context* Context()
{
    static secp256k1_context* const ctx = [] {
        secp256k1_context* c = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        unsigned char seed[32];
        GetStrongRandBytes(seed);
        const int ok = secp256k1_context_randomize(c, seed);
        assert(ok);
        return c;
    }();
    return ctx;
}

const secp256k1_generator* ScripGenerator()
{
    static const secp256k1_generator gen = [] {
        secp256k1_generator g;
        const int ok = secp256k1_generator_generate(Context(), &g, ScripAssetId().begin());
        assert(ok);
        return g;
    }();
    return &gen;
}

/** The ECDH nonce between a secret key and a public key (SHA256 of the shared point). */
bool SharedNonce(const unsigned char* seckey, const std::vector<unsigned char>& pubkey_bytes, unsigned char nonce[32])
{
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(Context(), &pubkey, pubkey_bytes.data(), pubkey_bytes.size())) return false;
    return secp256k1_ecdh(Context(), nonce, &pubkey, seckey, nullptr, nullptr);
}

uint256 MasterBlindingKey(CWallet& wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    if (!wallet.m_ct_master_key) {
        uint256 key;
        GetStrongRandBytes(key);
        WalletBatch(wallet.GetDatabase()).WriteCTMasterKey(key);
        wallet.m_ct_master_key = key;
    }
    return *wallet.m_ct_master_key;
}
} // namespace

CKey BlindingKey(CWallet& wallet, const CScript& program)
{
    AssertLockHeld(wallet.cs_wallet);
    static const HashWriter HASHER{TaggedHash("Freicoin/blinding-key")};
    const uint256 master = MasterBlindingKey(wallet);
    CKey key;
    for (uint32_t counter = 0; !key.IsValid(); ++counter) {
        HashWriter h{HASHER};
        h << master << program << counter;
        const uint256 k = h.GetSHA256();
        key.Set(k.begin(), k.end(), /*fCompressedIn=*/true);
    }
    return key;
}

Blind RandomBlind()
{
    Blind blind;
    do {
        GetStrongRandBytes(blind);
    } while (!secp256k1_ec_seckey_verify(Context(), blind.data()));
    return blind;
}

std::optional<Blind> BalancingBlind(const std::vector<Blind>& positive, const std::vector<Blind>& negative)
{
    std::vector<const unsigned char*> blinds;
    for (const Blind& b : positive) blinds.push_back(b.data());
    for (const Blind& b : negative) blinds.push_back(b.data());
    if (blinds.empty()) return std::nullopt;
    Blind out;
    if (!secp256k1_pedersen_blind_sum(Context(), out.data(), blinds.data(), blinds.size(), positive.size())) return std::nullopt;
    if (std::all_of(out.begin(), out.end(), [](unsigned char c) { return c == 0; })) return std::nullopt;
    return out;
}

bool MakeConfidentialOutput(CAmount value, const Blind& blind, const CScript& program, const CPubKey& recipient_blinding_key,
                            CTxOut& out, std::vector<unsigned char>& proof)
{
    if (value < 0 || value > MAX_MONEY || !recipient_blinding_key.IsFullyValid()) return false;
    secp256k1_context* ctx = Context();
    secp256k1_pedersen_commitment commit;
    if (!secp256k1_pedersen_commit(ctx, &commit, blind.data(), static_cast<uint64_t>(value), ScripGenerator())) return false;
    ConfidentialOutput c;
    if (!secp256k1_pedersen_commitment_serialize(ctx, c.commitment.data(), &commit)) return false;
    // an ephemeral key, whose ECDH with the recipient's blinding key seeds the proof
    CKey ephemeral;
    ephemeral.MakeNewKey(/*fCompressed=*/true);
    const CPubKey ephemeral_pub = ephemeral.GetPubKey();
    std::copy(ephemeral_pub.begin(), ephemeral_pub.end(), c.nonce_key.begin());
    unsigned char nonce[32];
    if (!SharedNonce(UCharCast(ephemeral.begin()), std::vector<unsigned char>(recipient_blinding_key.begin(), recipient_blinding_key.end()), nonce)) return false;
    proof.resize(MAX_CONFIDENTIAL_PROOF_SIZE);
    size_t plen = proof.size();
    if (!secp256k1_rangeproof_sign(ctx, proof.data(), &plen, 0, &commit, blind.data(), nonce, 0, PROOF_MIN_BITS, static_cast<uint64_t>(value),
                                   nullptr, 0, program.data(), program.size(), ScripGenerator())) {
        return false;
    }
    proof.resize(plen);
    out = CTxOut(0, CScript(program) << ConfidentialExtension(c));
    return true;
}

bool UnblindOutput(CWallet& wallet, const CTxOut& out, const std::vector<unsigned char>& proof, CAmount& value, Blind& blind)
{
    AssertLockHeld(wallet.cs_wallet);
    ConfidentialOutput c;
    if (GetConfidentialOutput(out, c) != ConfidentialOutputType::CONFIDENTIAL || proof.empty()) return false;
    secp256k1_context* ctx = Context();
    secp256k1_pedersen_commitment commit;
    if (!secp256k1_pedersen_commitment_parse(ctx, &commit, c.commitment.data())) return false;
    const CScript program = out.scriptPubKey.WithoutExtension();
    const CKey key = BlindingKey(wallet, program);
    unsigned char nonce[32];
    if (!SharedNonce(UCharCast(key.begin()), std::vector<unsigned char>(c.nonce_key.begin(), c.nonce_key.end()), nonce)) return false;
    uint64_t v = 0, min_value = 0, max_value = 0;
    unsigned char message[4096];
    size_t message_len = sizeof(message);
    if (!secp256k1_rangeproof_rewind(ctx, blind.data(), &v, message, &message_len, nonce, &min_value, &max_value, &commit,
                                     proof.data(), proof.size(), program.data(), program.size(), ScripGenerator())) {
        return false;
    }
    if (v > static_cast<uint64_t>(MAX_MONEY)) return false;
    value = static_cast<CAmount>(v);
    return true;
}
} // namespace wallet

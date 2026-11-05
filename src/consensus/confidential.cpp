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

#include <consensus/confidential.h>

#include <coins.h>
#include <consensus/assets.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <script/script.h>

#include <secp256k1.h>
#include <secp256k1_generator.h>
#include <secp256k1_rangeproof.h>

#include <algorithm>
#include <cassert>

namespace {
/** Verification needs a context that is not the static one; it is only read. */
const secp256k1_context* Context()
{
    static secp256k1_context* const ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    return ctx;
}

/** The generator scrip amounts are committed to: derived from its asset id, so assets
 *  without demurrage could be confidential the same way. */
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
} // namespace

ConfidentialOutputType GetConfidentialOutput(const CTxOut& txout, ConfidentialOutput& out)
{
    std::vector<unsigned char> ext;
    if (!txout.scriptPubKey.GetExtensionOutput(ext) || ext.empty() || ext[0] != EXTENSION_CONFIDENTIAL) {
        return ConfidentialOutputType::NONE;
    }
    if (ext.size() != CONFIDENTIAL_EXTENSION_SIZE) {
        return ConfidentialOutputType::MALFORMED;
    }
    std::copy(ext.begin() + 1, ext.begin() + 34, out.commitment.begin());
    std::copy(ext.begin() + 34, ext.end(), out.nonce_key.begin());
    return ConfidentialOutputType::CONFIDENTIAL;
}

std::vector<unsigned char> ConfidentialExtension(const ConfidentialOutput& out)
{
    std::vector<unsigned char> ext;
    ext.reserve(CONFIDENTIAL_EXTENSION_SIZE);
    ext.push_back(EXTENSION_CONFIDENTIAL);
    ext.insert(ext.end(), out.commitment.begin(), out.commitment.end());
    ext.insert(ext.end(), out.nonce_key.begin(), out.nonce_key.end());
    return ext;
}

int64_t ConfidentialProofBits(const CTransaction& tx)
{
    int64_t bits = 0;
    for (size_t n = 0; n < tx.vout.size(); ++n) {
        ConfidentialOutput c;
        if (GetConfidentialOutput(tx.vout[n], c) == ConfidentialOutputType::NONE) continue;
        int exp = 0, mantissa = 0;
        uint64_t min_value = 0, max_value = 0;
        if (n < tx.output_witness.size() && tx.output_witness[n].stack.size() == 1 && !tx.output_witness[n].stack[0].empty() &&
            secp256k1_rangeproof_info(Context(), &exp, &mantissa, &min_value, &max_value,
                                      tx.output_witness[n].stack[0].data(), tx.output_witness[n].stack[0].size())) {
            bits += std::max(mantissa, 1);
        } else {
            bits += 64;
        }
    }
    return bits;
}

bool Consensus::CheckTxConfidential(const CTransaction& tx, const CCoinsViewCache& inputs, int confidential_height,
                                    CAmount explicit_in, CAmount explicit_out, bool* has_confidential, TxValidationState& state)
{
    *has_confidential = false;
    const secp256k1_context* ctx = Context();
    std::vector<secp256k1_pedersen_commitment> ins, outs;

    for (size_t n = 0; n < tx.vout.size(); ++n) {
        const CTxOut& txout = tx.vout[n];
        ConfidentialOutput c;
        const ConfidentialOutputType type = GetConfidentialOutput(txout, c);
        if (type == ConfidentialOutputType::NONE) continue;
        if (type == ConfidentialOutputType::MALFORMED) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ct-malformed");
        }
        if (tx.IsCoinBase()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ct-coinbase");
        }
        if (txout.GetReferenceValue() != 0) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ct-nonzero-value");
        }
        secp256k1_pedersen_commitment commit;
        if (!secp256k1_pedersen_commitment_parse(ctx, &commit, c.commitment.data())) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ct-commitment");
        }
        // the range proof: the output witness's single item, bound to the output's program
        if (n >= tx.output_witness.size() || tx.output_witness[n].stack.size() != 1) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ct-proof-missing");
        }
        const std::vector<unsigned char>& proof = tx.output_witness[n].stack[0];
        if (proof.empty() || proof.size() > MAX_CONFIDENTIAL_PROOF_SIZE) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ct-proof-size");
        }
        const CScript program = txout.scriptPubKey.WithoutExtension();
        uint64_t min_value = 0, max_value = 0;
        if (!secp256k1_rangeproof_verify(ctx, &min_value, &max_value, &commit, proof.data(), proof.size(),
                                         program.data(), program.size(), ScripGenerator())) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ct-proof");
        }
        if (max_value > static_cast<uint64_t>(MAX_MONEY)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ct-range");
        }
        outs.push_back(commit);
    }

    if (!tx.IsCoinBase()) {
        for (const CTxIn& txin : tx.vin) {
            const Coin& coin = inputs.AccessCoin(txin.prevout);
            ConfidentialOutput c;
            if (GetConfidentialOutput(coin.out, c) != ConfidentialOutputType::CONFIDENTIAL) continue;
            // A commitment made before the rules applied was never checked: worthless.
            if (static_cast<int>(coin.nHeight) < confidential_height) continue;
            secp256k1_pedersen_commitment commit;
            if (!secp256k1_pedersen_commitment_parse(ctx, &commit, c.commitment.data())) {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ct-commitment");
            }
            ins.push_back(commit);
        }
    }

    if (ins.empty() && outs.empty()) return true;
    *has_confidential = true;

    // The explicit scrip on the side it is on, committed with no blinding.
    const CAmount net = explicit_in - explicit_out;
    if (net != 0) {
        static const unsigned char zero_blind[32] = {0};
        secp256k1_pedersen_commitment commit;
        if (!secp256k1_pedersen_commit(ctx, &commit, zero_blind, static_cast<uint64_t>(net > 0 ? net : -net), ScripGenerator())) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ct-balance");
        }
        (net > 0 ? ins : outs).push_back(commit);
    }
    std::vector<const secp256k1_pedersen_commitment*> pins, pouts;
    for (const auto& c : ins) pins.push_back(&c);
    for (const auto& c : outs) pouts.push_back(&c);
    // the arrays may be empty but not null
    const secp256k1_pedersen_commitment* const none = nullptr;
    if (!secp256k1_pedersen_verify_tally(ctx, pins.empty() ? &none : pins.data(), pins.size(),
                                         pouts.empty() ? &none : pouts.data(), pouts.size())) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-ct-balance");
    }
    return true;
}

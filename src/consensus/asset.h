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

#ifndef FREICOIN_CONSENSUS_ASSET_H
#define FREICOIN_CONSENSUS_ASSET_H

// nVersion=3-lite: fungible user-issued assets with per-asset demurrage. An output's 20-byte
// asset tag (CTxOut::assetTag; null = the host currency, freicoin) identifies its asset; this
// registry maps a tag to that asset's monetary policy, built from asset-definition txs. Mirrors
// core/assets.mjs + core/nv3chain.mjs in the freicoin-wallet reference model.

#include <hash.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace Consensus {

/** An asset's monetary policy. Rate = 2^-shift per block: DEMURRAGE (melts, shift=20 is the
 *  host currency) or INTEREST (grows — deferred, a wider representation is needed). */
struct AssetParams {
    uint8_t shift{20};
    bool interest{false};
    uint64_t granularity{1};
    /** Compressed pubkey (33 bytes) whose ECDSA approval every movement of this asset
     *  requires; empty = permissionless. Committed in the asset id (part of the def). */
    std::vector<unsigned char> authorizer;

    SERIALIZE_METHODS(AssetParams, obj) { READWRITE(obj.shift, obj.interest, obj.granularity, obj.authorizer); }
};

/** Canonical asset id = RIPEMD160(SHA256(canonical definition bytes)) — matches the model. */
inline uint160 AssetIdFromDef(const std::vector<unsigned char>& def) { return Hash160(def); }

/** Tag -> params, built from asset-definition txs as blocks connect. The host currency (null
 *  tag) is implicit and always {shift=20, demurrage, granularity=1}. */
class AssetRegistry {
    std::map<uint160, AssetParams> m_defs;
public:
    bool IsKnown(const uint160& tag) const { return tag.IsNull() || m_defs.count(tag) != 0; }
    AssetParams Get(const uint160& tag) const {
        if (tag.IsNull()) return AssetParams{20, false, 1};
        auto it = m_defs.find(tag);
        return it != m_defs.end() ? it->second : AssetParams{};
    }
    void Define(const uint160& tag, const AssetParams& p) { m_defs[tag] = p; }
    void Undefine(const uint160& tag) { m_defs.erase(tag); }
    size_t Size() const { return m_defs.size(); }

    // Persisted as datadir/assets.dat so definitions survive a node restart (an in-memory-only
    // registry would make every previously defined asset unknown — and its coins unspendable —
    // until a -reindex rebuilt it from the definition txs).
    SERIALIZE_METHODS(AssetRegistry, obj) { READWRITE(obj.m_defs); }
};

// The canonical definition byte string: shift(1) | flags(1) | granularity(8, LE) |
// contractHash(32) | [authorizer pubkey(33), only when flags bit 2 is set]. The id hashes the
// WHOLE def, so the authorizer is committed in the asset id (changing it = another asset).
static constexpr size_t ASSET_DEF_SIZE = 1 + 1 + 8 + 32;
static constexpr size_t ASSET_AUTHORIZER_SIZE = 33;
// Magic prefix marking an asset-definition OP_RETURN payload.
inline const unsigned char ASSET_DEF_MAGIC[4] = { 'F', 'R', 'A', '1' };
// Prefix of the digest an authorizer signs: SHA256d("FRAPPROV" || txid || tag).
inline const unsigned char ASSET_APPROVAL_TAG[8] = { 'F', 'R', 'A', 'P', 'P', 'R', 'O', 'V' };
// Magic prefix marking the two-sided token-reveal OP_RETURN payload (see ParseTokenReveal).
inline const unsigned char TOKEN_REVEAL_MAGIC[4] = { 'F', 'R', 'T', '1' };

/** The 32-byte commitment to a token set: double-SHA256 of its canonical serialization
 *  (compactSize(n) ++ n × (compactSize(len) ++ bytes)). The default vector formatter emits
 *  exactly that, so this byte-matches core/asset-spk.mjs tokenSetHash. */
inline uint256 TokenSetHash(const std::vector<std::vector<unsigned char>>& tokens)
{
    HashWriter ss;
    ss << tokens;
    return ss.GetHash();
}

/** If this tx declares a new asset (an OP_RETURN output carrying the magic + a canonical
 *  definition), return its {tag, params}. A tx defines at most one asset (first match wins). */
inline std::optional<std::pair<uint160, AssetParams>> ParseAssetDefinition(const CTransaction& tx)
{
    for (const CTxOut& o : tx.vout) {
        CScript::const_iterator pc = o.scriptPubKey.begin();
        opcodetype op;
        std::vector<unsigned char> data;
        if (!o.scriptPubKey.GetOp(pc, op) || op != OP_RETURN) continue;
        if (!o.scriptPubKey.GetOp(pc, op, data)) continue;
        if (data.size() != 4 + ASSET_DEF_SIZE && data.size() != 4 + ASSET_DEF_SIZE + ASSET_AUTHORIZER_SIZE) continue;
        if (!std::equal(std::begin(ASSET_DEF_MAGIC), std::end(ASSET_DEF_MAGIC), data.begin())) continue;
        const std::vector<unsigned char> def(data.begin() + 4, data.end());   // the canonical def bytes
        AssetParams p;
        p.shift = def[0];
        p.interest = (def[1] & 1) != 0;
        // The kernels are only defined for 1 <= shift <= 64 (a larger shift under-shifts the
        // fixed-point base — UB). A payload outside the range is simply not a definition.
        if (p.shift < 1 || p.shift > 64) continue;
        // Authorizer flag (bit 2) and the appended pubkey must agree, or it's not a definition.
        const bool has_auth = (def[1] & 2) != 0;
        if (has_auth != (def.size() == ASSET_DEF_SIZE + ASSET_AUTHORIZER_SIZE)) continue;
        if (has_auth) {
            // Only compressed pubkeys (0x02/0x03) name an authorizer.
            if (def[ASSET_DEF_SIZE] != 0x02 && def[ASSET_DEF_SIZE] != 0x03) continue;
            p.authorizer.assign(def.begin() + ASSET_DEF_SIZE, def.end());
        }
        uint64_t g = 0;
        for (int i = 0; i < 8; ++i) g |= static_cast<uint64_t>(def[2 + i]) << (8 * i);
        p.granularity = g ? g : 1;
        return std::make_pair(AssetIdFromDef(def), p);
    }
    return std::nullopt;
}

/** Parse the tx's single OP_RETURN "FRT1" two-sided token reveal into per-output and per-input
 *  token sets. Payload = FRT1 ++ <output section> ++ <input section>, each section
 *  = compactSize(n) ++ n × ( compactSize(index) ++ compactSize(count) ++ count × varbytes ).
 *  The OUTPUT section names this tx's committed outputs; the INPUT section names the committed
 *  coins being spent (the chainstate keeps only the hash, so the spender reveals them). Returns
 *  false on any malformation: more than one reveal, an index out of range or repeated, a short
 *  read, or trailing bytes. No FRT1 output ⇒ true with empty maps. Mirrors nv3wire.mjs
 *  parseTokenReveal; commitment verification is the caller's job (CheckTxInputs). */
inline bool ParseTokenReveal(const CTransaction& tx,
                             std::map<uint32_t, std::vector<std::vector<unsigned char>>>& out_toks,
                             std::map<uint32_t, std::vector<std::vector<unsigned char>>>& in_toks)
{
    out_toks.clear();
    in_toks.clear();
    bool seen = false;
    for (const CTxOut& o : tx.vout) {
        CScript::const_iterator pc = o.scriptPubKey.begin();
        opcodetype op;
        std::vector<unsigned char> data;
        if (!o.scriptPubKey.GetOp(pc, op) || op != OP_RETURN) continue;
        if (!o.scriptPubKey.GetOp(pc, op, data)) continue;
        if (data.size() < 4 || !std::equal(std::begin(TOKEN_REVEAL_MAGIC), std::end(TOKEN_REVEAL_MAGIC), data.begin())) continue;
        if (seen) return false;   // at most one reveal keeps validation deterministic
        seen = true;
        try {
            SpanReader r{std::span<const unsigned char>{data}.subspan(4)};
            auto section = [&](std::map<uint32_t, std::vector<std::vector<unsigned char>>>& m, size_t bound) {
                const uint64_t n = ReadCompactSize(r);
                for (uint64_t i = 0; i < n; ++i) {
                    const uint64_t idx = ReadCompactSize(r);
                    if (idx >= bound) throw std::ios_base::failure("token reveal index out of range");
                    if (m.count(static_cast<uint32_t>(idx))) throw std::ios_base::failure("duplicate token reveal index");
                    const uint64_t cnt = ReadCompactSize(r);
                    std::vector<std::vector<unsigned char>> toks;
                    toks.reserve(cnt);
                    for (uint64_t j = 0; j < cnt; ++j) {
                        std::vector<unsigned char> t;
                        r >> t;   // compactSize(len) ++ bytes
                        toks.push_back(std::move(t));
                    }
                    m.emplace(static_cast<uint32_t>(idx), std::move(toks));
                }
            };
            section(out_toks, tx.vout.size());
            section(in_toks, tx.vin.size());
            if (!r.empty()) return false;   // trailing bytes
        } catch (const std::exception&) {
            return false;
        }
    }
    return true;
}

} // namespace Consensus

#endif // FREICOIN_CONSENSUS_ASSET_H

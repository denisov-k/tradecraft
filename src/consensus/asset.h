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
};

// The canonical definition byte string: shift(1) | flags(1) | granularity(8, LE) | contractHash(32).
static constexpr size_t ASSET_DEF_SIZE = 1 + 1 + 8 + 32;
// Magic prefix marking an asset-definition OP_RETURN payload.
inline const unsigned char ASSET_DEF_MAGIC[4] = { 'F', 'R', 'A', '1' };

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
        if (data.size() != 4 + ASSET_DEF_SIZE) continue;
        if (!std::equal(std::begin(ASSET_DEF_MAGIC), std::end(ASSET_DEF_MAGIC), data.begin())) continue;
        const std::vector<unsigned char> def(data.begin() + 4, data.end());   // the canonical def bytes
        AssetParams p;
        p.shift = def[0];
        p.interest = (def[1] & 1) != 0;
        uint64_t g = 0;
        for (int i = 0; i < 8; ++i) g |= static_cast<uint64_t>(def[2 + i]) << (8 * i);
        p.granularity = g ? g : 1;
        return std::make_pair(AssetIdFromDef(def), p);
    }
    return std::nullopt;
}

} // namespace Consensus

#endif // FREICOIN_CONSENSUS_ASSET_H

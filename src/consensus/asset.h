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
#include <uint256.h>

#include <cstdint>
#include <map>
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

} // namespace Consensus

#endif // FREICOIN_CONSENSUS_ASSET_H

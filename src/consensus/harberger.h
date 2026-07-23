// Copyright (c) 2026 The Freicoin Developers
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of version 3 of the GNU Affero General Public License as published
// by the Free Software Foundation.  See <https://www.gnu.org/licenses/>.

// Freiland Harberger covenant (docs/freiland-covenant-spec.md, variant A). A HOST-FRC witness-
// version-2 output whose value IS the melting deposit — asset_pv(value) is the current forced-sale
// price V. Old nodes see witness version 2 as anyone-can-spend (softfork-compatible); NEW nodes
// enforce the covenant (this decoder is the first, rule-free step: it only parses the format,
// mirroring the JS model in core/asset-spk.mjs).

#ifndef FREICOIN_CONSENSUS_HARBERGER_H
#define FREICOIN_CONSENSUS_HARBERGER_H

#include <consensus/amount.h>
#include <script/script.h>
#include <uint256.h>

#include <cstdint>
#include <vector>

namespace Consensus {

/** A decoded Harberger covenant output. */
struct HarbergerCovenant {
    uint256 nameHash;   //!< sha256 of the name — the consensus name-registry key
    uint160 owner;      //!< hash160 the forced-sale price V is paid to (0014{owner})
    CAmount floorV{0};  //!< Gesell dust floor (kria); below this the name lapses
};

//! Exact serialized length of a Harberger scriptPubKey.
static constexpr size_t HARBERGER_SPK_SIZE = 1 + 1 + 32 + 1 + 20 + 1 + 8 + 1; // 65

/** Decode a Harberger covenant output. Exact wire form (see spec §3):
 *
 *    OP_1 0x20 <nameHash:32> 0x14 <owner:20> 0x08 <floorV:8 LE> OP_3
 *    ^witver-2  ^program=nameHash  ^suffix: owner + floorV        ^HRBG ext-version marker
 *
 *  Returns true and fills `out` iff `spk` is exactly this form. Pure decode: no consensus effect,
 *  no registry, no validation — a byte-for-byte mirror of decodeAssetSpk()'s version-3 branch. */
inline bool ParseHarbergerOutput(const CScript& spk, HarbergerCovenant& out)
{
    if (spk.size() != HARBERGER_SPK_SIZE) return false;
    const unsigned char* p = spk.data();
    if (p[0]  != OP_1)  return false;   // witness version 2 (unknown to old nodes ⇒ anyone-can-spend)
    if (p[1]  != 0x20)  return false;   // push 32: nameHash (the witness program)
    if (p[34] != 0x14)  return false;   // push 20: owner
    if (p[55] != 0x08)  return false;   // push 8:  floorV
    if (p[64] != OP_3)  return false;   // trailing extended-output version = HRBG
    out.nameHash = uint256(std::vector<unsigned char>(p + 2, p + 34));
    out.owner = uint160(std::vector<unsigned char>(p + 35, p + 55));
    CAmount floorV = 0;
    for (int i = 7; i >= 0; --i) floorV = (floorV << 8) | static_cast<CAmount>(p[56 + i]);
    out.floorV = floorV;
    return true;
}

/** True iff `spk` is a well-formed Harberger covenant output. */
inline bool IsHarbergerOutput(const CScript& spk)
{
    HarbergerCovenant tmp;
    return ParseHarbergerOutput(spk, tmp);
}

} // namespace Consensus

#endif // FREICOIN_CONSENSUS_HARBERGER_H

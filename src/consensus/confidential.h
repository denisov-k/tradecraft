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

#ifndef FREICOIN_CONSENSUS_CONFIDENTIAL_H
#define FREICOIN_CONSENSUS_CONFIDENTIAL_H

#include <consensus/amount.h>

#include <array>
#include <cstddef>
#include <vector>

class CCoinsViewCache;
class CTransaction;
class CTxOut;
class TxValidationState;

/** Confidential scrip (Forward Blocks, section XI-B): an output whose amount of scrip is
 *  hidden in a Pedersen commitment, proven in range by a range proof in the output's
 *  witness.  Scrip only: freicoin and assets with demurrage decay by a fractional
 *  factor, which a commitment cannot follow without revealing the amount. */
static constexpr unsigned char EXTENSION_CONFIDENTIAL = 0x06;
/** The extension: version, Pedersen commitment, the sender's ephemeral public key (for
 *  the recipient's ECDH, which recovers the amount from the range proof). */
static constexpr size_t CONFIDENTIAL_EXTENSION_SIZE = 1 + 33 + 33;
/** The largest range proof accepted (the rangeproof module's own bound). */
static constexpr size_t MAX_CONFIDENTIAL_PROOF_SIZE = 5134;

struct ConfidentialOutput {
    std::array<unsigned char, 33> commitment{};
    std::array<unsigned char, 33> nonce_key{};
};

enum class ConfidentialOutputType {
    NONE,         //!< no confidential extension
    CONFIDENTIAL, //!< a well-formed one (the commitment is not parsed yet)
    MALFORMED,    //!< one of the wrong size
};

/** Read the confidential extension of an output, if it has one. */
ConfidentialOutputType GetConfidentialOutput(const CTxOut& txout, ConfidentialOutput& out);

/** The extension push of a confidential output. */
std::vector<unsigned char> ConfidentialExtension(const ConfidentialOutput& out);

namespace Consensus {
/** Check the confidential scrip of a transaction (once the confidential rules apply).
 *  Confidential outputs hold no freicoin, each has a valid range proof (as the single
 *  item of its output witness, bound to the output's program, proving at most
 *  MAX_MONEY), and the commitments of the confidential inputs plus the explicit scrip
 *  in (`explicit_in`: scrip inputs and peg-ins) equal those of the confidential outputs
 *  plus the explicit scrip out (`explicit_out`: scrip outputs, peg-outs and fees).
 *  Confidential inputs created below `confidential_height` were never checked and count
 *  for nothing.  `*has_confidential` tells whether the balance replaced the explicit
 *  check of scrip. */
[[nodiscard]] bool CheckTxConfidential(const CTransaction& tx, const CCoinsViewCache& inputs, int confidential_height,
                                       CAmount explicit_in, CAmount explicit_out, bool* has_confidential, TxValidationState& state);
} // namespace Consensus

#endif // FREICOIN_CONSENSUS_CONFIDENTIAL_H

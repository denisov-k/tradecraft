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

#ifndef FREICOIN_WALLET_CONFIDENTIAL_H
#define FREICOIN_WALLET_CONFIDENTIAL_H

#include <consensus/amount.h>
#include <key.h>
#include <pubkey.h>
#include <script/script.h>

#include <array>
#include <vector>

class CTxOut;

namespace wallet {
class CWallet;

/** A blinding factor (a scalar, 32 bytes big endian). */
using Blind = std::array<unsigned char, 32>;

/** The key that blinds the confidential scrip paid to `program`, an output program of
 *  this wallet: derived from the wallet's master blinding key (created on first use and
 *  kept unencrypted, as it reveals amounts but spends nothing) and the program. */
CKey BlindingKey(CWallet& wallet, const CScript& program);

/** A random blinding factor. */
Blind RandomBlind();

/** The blinding factor that balances `positive` against `negative` (sum of the first
 *  minus sum of the second), or nullopt if it is zero. */
std::optional<Blind> BalancingBlind(const std::vector<Blind>& positive, const std::vector<Blind>& negative);

/** A confidential scrip output of `value` blinded by `blind`, to `program`, and its range
 *  proof (bound to the program): the recipient's blinding public key and an ephemeral key
 *  give the nonce from which the recipient recovers the value and the blinding factor. */
bool MakeConfidentialOutput(CAmount value, const Blind& blind, const CScript& program, const CPubKey& recipient_blinding_key,
                            CTxOut& out, std::vector<unsigned char>& proof);

/** Recover the value and blinding factor of a confidential output paid to this wallet,
 *  from its range proof (its output witness's item). */
bool UnblindOutput(CWallet& wallet, const CTxOut& out, const std::vector<unsigned char>& proof, CAmount& value, Blind& blind);
} // namespace wallet

#endif // FREICOIN_WALLET_CONFIDENTIAL_H

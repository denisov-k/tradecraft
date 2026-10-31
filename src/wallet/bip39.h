// Copyright (c) 2026 The Freicoin developers
// Distributed under the terms of the GNU Affero General Public License v3.

#ifndef FREICOIN_WALLET_BIP39_H
#define FREICOIN_WALLET_BIP39_H

#include <support/allocators/secure.h>

#include <optional>
#include <string>
#include <vector>

namespace wallet {
namespace bip39 {

using SecureBytes = std::vector<unsigned char, secure_allocator<unsigned char>>;

//! Encode 16..32 bytes of entropy (multiple of 4) as a BIP39 English mnemonic.
SecureString EntropyToMnemonic(const SecureBytes& entropy);

//! A fresh mnemonic from the strong RNG. 12 words (128 bits) by default.
SecureString GenerateMnemonic(int words = 12);

//! Lower-case, trim and collapse whitespace. Returns nullopt if a word is not
//! in the wordlist, the word count is not 12/15/18/21/24, or the checksum fails.
std::optional<SecureString> NormalizeAndCheck(const SecureString& mnemonic);

//! PBKDF2-HMAC-SHA512, 2048 rounds, salt "mnemonic" + passphrase (BIP39).
//! Expects an already-normalized mnemonic. Returns the 64-byte seed.
SecureBytes MnemonicToSeed(const SecureString& mnemonic, const SecureString& passphrase = "");

} // namespace bip39
} // namespace wallet

#endif // FREICOIN_WALLET_BIP39_H

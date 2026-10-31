// Copyright (c) 2026 The Freicoin developers
// Distributed under the terms of the GNU Affero General Public License v3.

#include <wallet/bip39.h>
#include <wallet/bip39_english.h>

#include <crypto/hmac_sha512.h>
#include <crypto/sha256.h>
#include <random.h>
#include <support/cleanse.h>

#include <cstring>
#include <stdexcept>

namespace wallet {
namespace bip39 {

static int WordIndex(const std::string& w)
{
    // The list is sorted; binary search.
    int lo = 0, hi = 2047;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = std::strcmp(w.c_str(), BIP39_ENGLISH[mid]);
        if (c == 0) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

SecureString EntropyToMnemonic(const SecureBytes& entropy)
{
    const size_t ent_bits = entropy.size() * 8;
    if (entropy.size() < 16 || entropy.size() > 32 || entropy.size() % 4 != 0) {
        throw std::invalid_argument("BIP39 entropy must be 16..32 bytes, a multiple of 4");
    }
    const size_t cs_bits = ent_bits / 32;
    unsigned char hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(entropy.data(), entropy.size()).Finalize(hash);

    // bits = entropy || first cs_bits of hash
    SecureBytes bits(entropy.begin(), entropy.end());
    bits.push_back(hash[0]); // cs_bits <= 8
    memory_cleanse(hash, sizeof(hash));

    const size_t n_words = (ent_bits + cs_bits) / 11;
    SecureString out;
    for (size_t i = 0; i < n_words; ++i) {
        int idx = 0;
        for (size_t b = 0; b < 11; ++b) {
            const size_t bit = i * 11 + b;
            idx = (idx << 1) | ((bits[bit / 8] >> (7 - (bit % 8))) & 1);
        }
        if (i) out += ' ';
        out += BIP39_ENGLISH[idx];
    }
    return out;
}

SecureString GenerateMnemonic(int words)
{
    if (words != 12 && words != 15 && words != 18 && words != 21 && words != 24) {
        throw std::invalid_argument("word count must be 12, 15, 18, 21 or 24");
    }
    SecureBytes entropy(words * 4 / 3); // 12 -> 16 bytes, 24 -> 32 bytes
    GetStrongRandBytes(entropy);
    return EntropyToMnemonic(entropy);
}

std::optional<SecureString> NormalizeAndCheck(const SecureString& mnemonic)
{
    std::vector<std::string> words;
    std::string cur;
    for (char ch : mnemonic) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else if (c >= 'A' && c <= 'Z') {
            cur += char(c - 'A' + 'a');
        } else if (c >= 'a' && c <= 'z') {
            cur += char(c);
        } else {
            memory_cleanse(cur.data(), cur.size());
            return std::nullopt; // English wordlist is plain a-z
        }
    }
    if (!cur.empty()) words.push_back(cur);
    memory_cleanse(cur.data(), cur.size());

    const size_t n = words.size();
    if (n != 12 && n != 15 && n != 18 && n != 21 && n != 24) return std::nullopt;

    const size_t total_bits = n * 11;
    const size_t cs_bits = total_bits / 33;
    const size_t ent_bytes = (total_bits - cs_bits) / 8;
    SecureBytes bits((total_bits + 7) / 8, 0);
    for (size_t i = 0; i < n; ++i) {
        int idx = WordIndex(words[i]);
        if (idx < 0) {
            for (auto& w : words) memory_cleanse(w.data(), w.size());
            return std::nullopt;
        }
        for (size_t b = 0; b < 11; ++b) {
            if ((idx >> (10 - b)) & 1) {
                const size_t bit = i * 11 + b;
                bits[bit / 8] |= (1 << (7 - (bit % 8)));
            }
        }
    }
    unsigned char hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(bits.data(), ent_bytes).Finalize(hash);
    const unsigned char want = hash[0] >> (8 - cs_bits);
    const unsigned char have = bits[ent_bytes] >> (8 - cs_bits);
    memory_cleanse(hash, sizeof(hash));

    SecureString out;
    for (size_t i = 0; i < n; ++i) {
        if (i) out += ' ';
        out += words[i];
        memory_cleanse(words[i].data(), words[i].size());
    }
    if (want != have) return std::nullopt;
    return out;
}

SecureBytes MnemonicToSeed(const SecureString& mnemonic, const SecureString& passphrase)
{
    // PBKDF2-HMAC-SHA512, one 64-byte block, 2048 iterations.
    SecureString salt = "mnemonic";
    salt += passphrase;
    const auto* pw = reinterpret_cast<const unsigned char*>(mnemonic.data());

    unsigned char u[CHMAC_SHA512::OUTPUT_SIZE];
    {
        CHMAC_SHA512 h(pw, mnemonic.size());
        h.Write(reinterpret_cast<const unsigned char*>(salt.data()), salt.size());
        const unsigned char block_index[4] = {0, 0, 0, 1};
        h.Write(block_index, 4);
        h.Finalize(u);
    }
    SecureBytes t(u, u + sizeof(u));
    for (int i = 1; i < 2048; ++i) {
        CHMAC_SHA512(pw, mnemonic.size()).Write(u, sizeof(u)).Finalize(u);
        for (size_t j = 0; j < sizeof(u); ++j) t[j] ^= u[j];
    }
    memory_cleanse(u, sizeof(u));
    return t;
}

} // namespace bip39
} // namespace wallet

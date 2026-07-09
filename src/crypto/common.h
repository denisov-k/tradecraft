// Copyright (c) 2014-2020 The Bitcoin Core developers
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

#ifndef FREICOIN_CRYPTO_COMMON_H
#define FREICOIN_CRYPTO_COMMON_H

#include <compat/endian.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>

template <typename B>
concept ByteType = std::same_as<B, unsigned char> || std::same_as<B, std::byte>;

template <ByteType B>
inline uint16_t ReadLE16(const B* ptr)
{
    uint16_t x;
    memcpy(&x, ptr, 2);
    return le16toh_internal(x);
}

template <ByteType B>
inline uint32_t ReadLE32(const B* ptr)
{
    uint32_t x;
    memcpy(&x, ptr, 4);
    return le32toh_internal(x);
}

template <ByteType B>
inline uint64_t ReadLE64(const B* ptr)
{
    uint64_t x;
    memcpy(&x, ptr, 8);
    return le64toh_internal(x);
}

template <ByteType B>
inline void WriteLE16(B* ptr, uint16_t x)
{
    uint16_t v = htole16_internal(x);
    memcpy(ptr, &v, 2);
}

template <ByteType B>
inline void WriteLE32(B* ptr, uint32_t x)
{
    uint32_t v = htole32_internal(x);
    memcpy(ptr, &v, 4);
}

template <ByteType B>
inline void WriteLE64(B* ptr, uint64_t x)
{
    uint64_t v = htole64_internal(x);
    memcpy(ptr, &v, 8);
}

template <ByteType B>
inline uint16_t ReadBE16(const B* ptr)
{
    uint16_t x;
    memcpy(&x, ptr, 2);
    return be16toh_internal(x);
}

template <ByteType B>
inline uint32_t ReadBE32(const B* ptr)
{
    uint32_t x;
    memcpy(&x, ptr, 4);
    return be32toh_internal(x);
}

template <ByteType B>
inline uint64_t ReadBE64(const B* ptr)
{
    uint64_t x;
    memcpy(&x, ptr, 8);
    return be64toh_internal(x);
}

template <ByteType B>
inline void WriteBE16(B* ptr, uint16_t x)
{
    uint16_t v = htobe16_internal(x);
    memcpy(ptr, &v, 2);
}

template <ByteType B>
inline void WriteBE32(B* ptr, uint32_t x)
{
    uint32_t v = htobe32_internal(x);
    memcpy(ptr, &v, 4);
}

template <ByteType B>
inline void WriteBE64(B* ptr, uint64_t x)
{
    uint64_t v = htobe64_internal(x);
    memcpy(ptr, &v, 8);
}

#endif // FREICOIN_CRYPTO_COMMON_H

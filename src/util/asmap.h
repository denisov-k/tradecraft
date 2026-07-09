// Copyright (c) 2019-2021 The Bitcoin Core developers
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

#ifndef FREICOIN_UTIL_ASMAP_H
#define FREICOIN_UTIL_ASMAP_H

#include <uint256.h>
#include <util/fs.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

uint32_t Interpret(std::span<const std::byte> asmap, std::span<const std::byte> ip);

bool SanityCheckAsmap(std::span<const std::byte> asmap, int bits);
/** Check standard asmap data (128 bits for IPv6) */
bool CheckStandardAsmap(std::span<const std::byte> data);

/** Read and check asmap from provided binary file */
std::vector<std::byte> DecodeAsmap(fs::path path);
/** Calculate the asmap version, a checksum identifying the asmap being used. */
uint256 AsmapVersion(std::span<const std::byte> data);

#endif // FREICOIN_UTIL_ASMAP_H

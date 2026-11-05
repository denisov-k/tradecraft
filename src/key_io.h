// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
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

#ifndef FREICOIN_KEY_IO_H
#define FREICOIN_KEY_IO_H

#include <addresstype.h>
#include <chainparams.h>
#include <key.h>
#include <pubkey.h>

#include <string>

CKey DecodeSecret(const std::string& str);
std::string EncodeSecret(const CKey& key);

CExtKey DecodeExtKey(const std::string& str);
std::string EncodeExtKey(const CExtKey& extkey);
CExtPubKey DecodeExtPubKey(const std::string& str);
std::string EncodeExtPubKey(const CExtPubKey& extpubkey);

std::string EncodeDestination(const CTxDestination& dest);

/** A confidential address: a native witness destination and the blinding public key the
 *  sender hides the amount with, in blech32m (Elements' longer-checksum bech32m, made for
 *  addresses this long) under the network's prefix with "c" before it (cfc1..., ctf1...,
 *  cfcrt1...). */
std::string EncodeConfidentialAddress(const CTxDestination& dest, const CPubKey& blinding_key);
/** Decode a confidential address; false if it is not one (of this network). */
bool DecodeConfidentialAddress(const std::string& str, CTxDestination& dest, CPubKey& blinding_key);
CTxDestination DecodeDestination(const std::string& str);
CTxDestination DecodeDestination(const std::string& str, std::string& error_msg, std::vector<int>* error_locations = nullptr);
bool IsValidDestinationString(const std::string& str);
bool IsValidDestinationString(const std::string& str, const CChainParams& params);

#endif // FREICOIN_KEY_IO_H

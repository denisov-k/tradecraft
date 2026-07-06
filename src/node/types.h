// Copyright (c) 2010-2021 The Bitcoin Core developers
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

//! @file node/types.h is a home for public enum and struct type definitions
//! that are used internally by node code, but also used externally by wallet,
//! mining or GUI code.
//!
//! This file is intended to define only simple types that do not have external
//! dependencies. More complicated types should be defined in dedicated header
//! files.

#ifndef FREICOIN_NODE_TYPES_H
#define FREICOIN_NODE_TYPES_H

#include <cstddef>
#include <policy/policy.h>
#include <script/script.h>

namespace node {
enum class TransactionError {
    OK, //!< No error
    MISSING_INPUTS,
    ALREADY_IN_UTXO_SET,
    MEMPOOL_REJECTED,
    MEMPOOL_ERROR,
    MAX_FEE_EXCEEDED,
    MAX_BURN_EXCEEDED,
    INVALID_PACKAGE,
};

struct BlockCreateOptions {
    /**
     * Set false to omit mempool transactions
     */
    bool use_mempool{true};
    /**
     * The default reserved weight for the fixed-size block header,
     * transaction count and coinbase transaction.
     */
    size_t block_reserved_weight{DEFAULT_BLOCK_RESERVED_WEIGHT};
    /**
     * The maximum additional sigops which the pool will add in coinbase
     * transaction outputs.
     */
    size_t coinbase_output_max_additional_sigops{400};
    /**
     * Script to put in the coinbase transaction. The default is an
     * anyone-can-spend dummy.
     *
     * Should only be used for tests, when the default doesn't suffice.
     *
     * Note that higher level code like the getblocktemplate RPC may omit the
     * coinbase transaction entirely. It's instead constructed by pool software
     * using fields like coinbasevalue, coinbaseaux and default_witness_commitment.
     * This software typically also controls the payout outputs, even for solo
     * mining.
     *
     * The size and sigops are not checked against
     * coinbase_max_additional_weight and coinbase_output_max_additional_sigops.
     */
    CScript coinbase_output_script{CScript() << OP_TRUE};
};
} // namespace node

#endif // FREICOIN_NODE_TYPES_H

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

#include <signet.h>

#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <hash.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <streams.h>
#include <uint256.h>
#include <util/check.h>
#include <util/log.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <utility>
#include <vector>

static constexpr uint8_t SIGNET_HEADER[4] = {0xec, 0xc7, 0xda, 0xa2};

static constexpr script_verify_flags BLOCK_SCRIPT_VERIFY_FLAGS = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_DERSIG;

static bool FetchAndClearCommitmentSection(const std::span<const uint8_t> header, CScript& witness_commitment, std::vector<uint8_t>& result)
{
    CScript replacement;
    bool found_header = false;
    result.clear();

    opcodetype opcode;
    CScript::const_iterator pc = witness_commitment.begin();
    std::vector<uint8_t> pushdata;
    while (witness_commitment.GetOp(pc, opcode, pushdata)) {
        if (pushdata.size() > 0) {
            if (!found_header && pushdata.size() > header.size() && std::ranges::equal(std::span{pushdata}.first(header.size()), header)) {
                // pushdata only counts if it has the header _and_ some data
                result.insert(result.end(), pushdata.begin() + header.size(), pushdata.end());
                pushdata.erase(pushdata.begin() + header.size(), pushdata.end());
                found_header = true;
            }
            replacement << pushdata;
        } else {
            replacement << opcode;
        }
    }

    if (found_header) witness_commitment = replacement;
    return found_header;
}

//! Hash a transaction's no-witness serialization with the trailing witness
//! commitment (path byte + Merkle root), if present, zeroed out — the same
//! stripping BlockWitnessMerkleRoot performs on the block-final transaction.
static uint256 CommitmentZeroedTxLeaf(DataStream& tx)
{
    if (tx.size() >= (1 + 32 + 4 + 8)            // <- 1 byte for witness path
        && tx[tx.size()-8-4] == std::byte{0x4b}  //   32 bytes for merkle root
        && tx[tx.size()-8-3] == std::byte{0x4a}  //    4 bytes for magic value
        && tx[tx.size()-8-2] == std::byte{0x49}  //    4 bytes for nLockTime
        && tx[tx.size()-8-1] == std::byte{0x48}) //    4 bytes for lock_height
    {                                            //      (end of transaction)
        std::fill_n(tx.end()-8-4-32-1, 33, std::byte{0});
    }
    uint256 leaf;
    CHash256()
        .Write({(unsigned char*)&tx[0], tx.size()})
        .Finalize(leaf);
    return leaf;
}

static uint256 ComputeModifiedMerkleRoot(const CMutableTransaction& cb, const CBlock& block)
{
    // In Freicoin the witness Merkle root's coinbase leaf covers the coinbase
    // outputs, one of which holds the signet solution.  The witness commitment
    // therefore cannot be fixed until AFTER the block is signed, so the data
    // being signed must be independent of it: the last transaction's leaf
    // (which carries the commitment — the coinbase itself in a one-tx block)
    // is hashed with the commitment bytes zeroed, mirroring
    // BlockWitnessMerkleRoot.  Signing order is then: sign -> attach solution
    // -> compute witness commitment.
    std::vector<uint256> leaves;
    leaves.reserve((block.vtx.size() + 1) & ~1ULL); // capacity rounded up to even
    if (block.vtx.size() == 1) {
        DataStream tx;
        tx << TX_NO_WITNESS(cb);
        leaves.push_back(CommitmentZeroedTxLeaf(tx));
    } else {
        leaves.push_back(cb.GetHash().ToUint256());
        for (size_t s = 1; s + 1 < block.vtx.size(); ++s) {
            leaves.push_back(block.vtx[s]->GetHash().ToUint256());
        }
        DataStream tx;
        tx << TX_NO_WITNESS(block.vtx.back());
        leaves.push_back(CommitmentZeroedTxLeaf(tx));
    }
    return ComputeMerkleRoot(std::move(leaves));
}

std::optional<SignetTxs> SignetTxs::Create(const CBlock& block, const CScript& challenge)
{
    CMutableTransaction tx_to_spend;
    tx_to_spend.version = 0;
    tx_to_spend.nLockTime = 0;
    tx_to_spend.lock_height = 0;
    tx_to_spend.vin.emplace_back(COutPoint(), CScript(OP_0), 0);
    tx_to_spend.vout.emplace_back(0, challenge);

    CMutableTransaction tx_spending;
    tx_spending.version = 0;
    tx_spending.nLockTime = 0;
    tx_spending.lock_height = 0;
    tx_spending.vin.emplace_back(COutPoint(), CScript(), 0);
    tx_spending.vout.emplace_back(0, CScript(OP_RETURN));

    // can't fill any other fields before extracting signet
    // responses from block coinbase tx

    // find and delete signet signature
    if (block.vtx.empty()) return std::nullopt; // no coinbase tx in block; invalid
    CMutableTransaction modified_cb(*block.vtx.at(0));

    if (!GetWitnessCommitment(block, nullptr, nullptr)) {
        return std::nullopt; // require a witness commitment
    }

    CScript& witness_commitment = modified_cb.vout.back().scriptPubKey;

    std::vector<uint8_t> signet_solution;
    if (!FetchAndClearCommitmentSection(SIGNET_HEADER, witness_commitment, signet_solution)) {
        // no signet solution -- allow this to support OP_TRUE as trivial block challenge
    } else {
        try {
            SpanReader v{signet_solution};
            v >> tx_spending.vin[0].scriptSig;
            v >> tx_spending.vin[0].scriptWitness.stack;
            if (!v.empty()) return std::nullopt; // extraneous data encountered
        } catch (const std::exception&) {
            return std::nullopt; // parsing error
        }
    }
    uint256 signet_merkle = ComputeModifiedMerkleRoot(modified_cb, block);

    std::vector<uint8_t> block_data;
    VectorWriter writer{block_data, 0};
    writer << block.nVersion;
    writer << block.hashPrevBlock;
    writer << signet_merkle;
    writer << block.nTime;
    tx_to_spend.vin[0].scriptSig << block_data;
    tx_spending.vin[0].prevout = COutPoint(tx_to_spend.GetHash(), 0);

    return SignetTxs{tx_to_spend, tx_spending};
}

// Signet block solution checker
bool CheckSignetBlockSolution(const CBlock& block, const Consensus::Params& consensusParams)
{
    if (block.GetHash() == consensusParams.hashGenesisBlock) {
        // genesis block solution is always valid
        return true;
    }

    const CScript challenge(consensusParams.signet_challenge.begin(), consensusParams.signet_challenge.end());
    const std::optional<SignetTxs> signet_txs = SignetTxs::Create(block, challenge);

    if (!signet_txs) {
        LogDebug(BCLog::VALIDATION, "CheckSignetBlockSolution: Errors in block (block solution parse failure)\n");
        return false;
    }

    const CScript& scriptSig = signet_txs->m_to_sign.vin[0].scriptSig;
    const CScriptWitness& witness = signet_txs->m_to_sign.vin[0].scriptWitness;

    PrecomputedTransactionData txdata;
    txdata.Init(signet_txs->m_to_sign, {{signet_txs->m_to_spend.vout[0], signet_txs->m_to_spend.lock_height}});
    TransactionSignatureChecker sigcheck(&signet_txs->m_to_sign, /* nInIn= */ 0, /* amountIn= */ signet_txs->m_to_spend.vout[0].GetReferenceValue(), signet_txs->m_to_spend.lock_height, txdata, MissingDataBehavior::ASSERT_FAIL);

    if (!VerifyScript(scriptSig, signet_txs->m_to_spend.vout[0].scriptPubKey, &witness, BLOCK_SCRIPT_VERIFY_FLAGS, sigcheck)) {
        LogDebug(BCLog::VALIDATION, "CheckSignetBlockSolution: Errors in block (block solution invalid)\n");
        return false;
    }
    return true;
}

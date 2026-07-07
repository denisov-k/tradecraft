// Copyright (c) 2024 The Bitcoin Core developers
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

#ifndef FREICOIN_INTERFACES_MINING_H
#define FREICOIN_INTERFACES_MINING_H

#include <consensus/amount.h>
#include <interfaces/types.h>
#include <node/types.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/time.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace node {
struct NodeContext;
} // namespace node

class BlockValidationState;
class CScript;

namespace interfaces {

//! Block template interface
class BlockTemplate
{
public:
    virtual ~BlockTemplate() = default;

    virtual CBlockHeader getBlockHeader() = 0;
    // Block contains a dummy coinbase transaction that should not be used.
    virtual CBlock getBlock() = 0;

    // Fees per transaction, not including coinbase transaction.
    virtual std::vector<CAmount> getTxFees() = 0;
    // Sigop cost per transaction, not including coinbase transaction.
    virtual std::vector<int64_t> getTxSigops() = 0;

    /** Return fields needed to construct a coinbase transaction */
    virtual node::CoinbaseTx getCoinbaseTx() = 0;

    //! Freicoin: whether the template ends with a block-final transaction.
    virtual bool hasBlockFinalTx() = 0;
    //! Freicoin: UTXO records for the block-final transaction inputs.
    virtual std::map<COutPoint, Coin> getBlockFinalTxCoinMap() = 0;

    /**
     * Compute merkle path to the coinbase transaction
     *
     * @return merkle path ordered from the deepest
     */
    virtual std::vector<uint256> getCoinbaseMerklePath() = 0;

    /**
     * Construct and broadcast the block. Modifies the template in place,
     * updating the fields listed below as well as the merkle root.
     *
     * @param[in] version version block header field
     * @param[in] timestamp time block header field (unix timestamp)
     * @param[in] nonce nonce block header field
     * @param[in] coinbase complete coinbase transaction (including witness)
     *
     * @note unlike the submitblock RPC, this method does NOT add the
     *       coinbase witness automatically.
     *
     * @returns if the block was processed, does not necessarily indicate validity.
     *
     * @note Returns true if the block is already known, which can happen if
     *       the solved block is constructed and broadcast by multiple nodes
     *       (e.g. both the miner who constructed the template and the pool).
     */
    virtual bool submitSolution(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase) = 0;

    /**
     * Waits for fees in the next block to rise, a new tip or the timeout.
     *
     * @param[in] options   Control the timeout (default forever) and by how much total fees
     *                      for the next block should rise (default infinite).
     *
     * @returns a new BlockTemplate or nothing if the timeout occurs.
     *
     * On testnet this will additionally return a template with difficulty 1 if
     * the tip is more than 20 minutes old.
     */
    virtual std::unique_ptr<BlockTemplate> waitNext(node::BlockWaitOptions options = {}) = 0;

    /**
     * Interrupts the current wait for the next block template.
    */
    virtual void interruptWait() = 0;
};

//! Interface giving clients (RPC, Stratum v2 Template Provider in the future)
//! ability to create block templates.
class Mining
{
public:
    virtual ~Mining() = default;

    //! If this chain is exclusively used for testing
    virtual bool isTestChain() = 0;

    //! Returns whether IBD is still in progress.
    virtual bool isInitialBlockDownload() = 0;

    //! Returns the hash and height for the tip of this chain
    virtual std::optional<BlockRef> getTip() = 0;

    /**
     * Waits for the connected tip to change. During node initialization, this will
     * wait until the tip is connected (regardless of `timeout`).
     *
     * @param[in] current_tip block hash of the current chain tip. Function waits
     *                        for the chain tip to differ from this.
     * @param[in] timeout     how long to wait for a new tip (default is forever)
     *
     * @retval BlockRef hash and height of the current chain tip after this call.
     * @retval std::nullopt if the node is shut down or interrupt() is called.
     */
    virtual std::optional<BlockRef> waitTipChanged(uint256 current_tip, MillisecondsDouble timeout = MillisecondsDouble::max()) = 0;

   /**
     * Construct a new block template.
     *
     * @param[in] options options for creating the block
     * @param[in] cooldown wait for tip to be connected and IBD to complete.
     *                     If the best header is ahead of the tip, wait for the
     *                     tip to catch up. It's recommended to disable this on
     *                     regtest and signets with only one miner, as these
     *                     could stall.
     * @retval BlockTemplate a block template.
     * @retval std::nullptr if the node is shut down or interrupt() is called.
     */
    virtual std::unique_ptr<BlockTemplate> createNewBlock(const node::BlockCreateOptions& options = {}, bool cooldown = true) = 0;

    /**
     * Interrupts createNewBlock and waitTipChanged.
     */
    virtual void interrupt() = 0;

    /**
     * Checks if a given block is valid.
     *
     * @param[in] block       the block to check
     * @param[in] options     verification options: the proof-of-work check can be
     *                        skipped in order to verify a template generated by
     *                        external software.
     * @param[out] reason     failure reason (BIP22)
     * @param[out] debug      more detailed rejection reason
     * @returns               whether the block is valid
     *
     * For signets the challenge verification is skipped when check_pow is false.
     */
    virtual bool checkBlock(const CBlock& block, const node::BlockCheckOptions& options, std::string& reason, std::string& debug) = 0;

    //! Get internal node context. Useful for RPC and testing,
    //! but not accessible across processes.
    virtual node::NodeContext* context() { return nullptr; }
};

//! Return implementation of Mining interface.
//!
//! @param[in] wait_loaded waits for chainstate data to be loaded before
//!                        returning. Used to prevent external clients from
//!                        being able to crash the node during startup.
std::unique_ptr<Mining> MakeMining(node::NodeContext& node, bool wait_loaded=true);

} // namespace interfaces

#endif // FREICOIN_INTERFACES_MINING_H

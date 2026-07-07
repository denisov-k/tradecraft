// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
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

#include <node/miner.h>

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <logging.h>
#include <node/context.h>
#include <node/kernel_notifications.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <util/moneystr.h>
#include <util/signalinterrupt.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <utility>
#include <numeric>

namespace node {

int64_t GetMinimumTime(const CBlockIndex* pindexPrev, const int64_t difficulty_adjustment_interval)
{
    int64_t min_time{pindexPrev->GetMedianTimePast() + 1};
    // Height of block to be mined.
    const int height{pindexPrev->nHeight + 1};
    // Account for BIP94 timewarp rule on all networks. This makes future
    // activation safer.
    if (height % difficulty_adjustment_interval == 0) {
        min_time = std::max<int64_t>(min_time, pindexPrev->GetBlockTime() - MAX_TIMEWARP);
    }
    return min_time;
}

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime{std::max<int64_t>(pindexPrev->GetMedianTimePast() + 1, TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()))};

    if (nOldTime < nNewTime) {
        pblock->nTime = nNewTime;
    }

    return nNewTime - nOldTime;
}

void RegenerateCommitments(CBlock& block, ChainstateManager& chainman)
{
    const CBlockIndex* prev_block = WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock));
    chainman.GenerateCoinbaseCommitment(block, prev_block);

    block.hashMerkleRoot = BlockMerkleRoot(block);
}

static BlockAssembler::Options ClampOptions(BlockAssembler::Options options)
{
    // Apply DEFAULT_BLOCK_RESERVED_WEIGHT when the caller left it unset.
    options.block_reserved_weight = std::clamp<size_t>(options.block_reserved_weight.value_or(DEFAULT_BLOCK_RESERVED_WEIGHT), MINIMUM_BLOCK_RESERVED_WEIGHT, MAX_BLOCK_WEIGHT);
    options.coinbase_output_max_additional_sigops = std::clamp<size_t>(options.coinbase_output_max_additional_sigops, 0, MAX_BLOCK_SIGOPS_COST);
    // Limit weight to between block_reserved_weight and MAX_BLOCK_WEIGHT for sanity:
    // block_reserved_weight can safely exceed -blockmaxweight, but the rest of the block template will be empty.
    options.nBlockMaxWeight = std::clamp<size_t>(options.nBlockMaxWeight, *options.block_reserved_weight, MAX_BLOCK_WEIGHT);
    return options;
}

BlockAssembler::BlockAssembler(Chainstate& chainstate, const CTxMemPool* mempool, const Options& options)
    : m_median_time_past{0},
      chainparams{chainstate.m_chainman.GetParams()},
      m_mempool{options.use_mempool ? mempool : nullptr},
      m_chainstate{chainstate},
      m_block_final_state{NO_BLOCK_FINAL_TX},
      m_options{ClampOptions(options)}
{
}

void ApplyArgsManOptions(const ArgsManager& args, BlockAssembler::Options& options)
{
    // Block resource limits
    options.nBlockMaxWeight = args.GetIntArg("-blockmaxweight", options.nBlockMaxWeight);
    if (const auto blockmintxfee{args.GetArg("-blockmintxfee")}) {
        if (const auto parsed{ParseMoney(*blockmintxfee)}) options.blockMinFeeRate = CFeeRate{*parsed};
    }
    options.print_modified_fee = args.GetBoolArg("-printpriority", options.print_modified_fee);
    if (!options.block_reserved_weight) {
        options.block_reserved_weight = args.GetIntArg("-blockreservedweight");
    }
}

void BlockAssembler::resetBlock()
{
    // Reserve space for fixed-size block header, txs count, and coinbase tx.
    nBlockWeight = *Assert(m_options.block_reserved_weight);
    nBlockSigOpsCost = m_options.coinbase_output_max_additional_sigops;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;

    m_median_time_past = 0;
    m_block_final_state = NO_BLOCK_FINAL_TX;
    m_block_final_tx_coin_map.clear();
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock()
{
    const auto time_start{SteadyClock::now()};

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction. It is skipped by the
    // getblocktemplate RPC and mining interface consumers must not use it.
    pblock->vtx.emplace_back();

    LOCK(::cs_main);
    CBlockIndex* pindexPrev = m_chainstate.m_chain.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = m_chainstate.m_chainman.m_versionbitscache.ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand()) {
        pblock->nVersion = gArgs.GetIntArg("-blockversion", pblock->nVersion);
    }

    pblock->nTime = TicksSinceEpoch<std::chrono::seconds>(NodeClock::now());
    m_median_time_past = pindexPrev->GetMedianTimePast();
    m_lock_time_cutoff = m_median_time_past;

    // Check if block-final tx rules are enforced. For the moment this
    // tracks just whether the soft-fork is active, but by the time we get
    // to transaction selection it will only be true if there is a
    // block-final transaction in this block template.
    if (DeploymentActiveAfter(pindexPrev, m_chainstate.m_chainman, Consensus::DEPLOYMENT_FINALTX)) {
        m_block_final_state = HAS_BLOCK_FINAL_TX;
    }

    // Check if this is the first block for which the block-final rules are
    // enforced, in which case all we need to do is add the initial
    // anyone-can-spend output.
    if ((m_block_final_state == HAS_BLOCK_FINAL_TX) && (!pindexPrev->pprev || !DeploymentActiveAfter(pindexPrev->pprev, m_chainstate.m_chainman, Consensus::DEPLOYMENT_FINALTX))) {
        m_block_final_state = INITIAL_BLOCK_FINAL_TXOUT;
    }

    // Otherwise we will need to check if the prior block-final transaction
    // was a coinbase and if insufficient blocks have occured for it to mature.
    BlockFinalTxEntry final_tx;
    if (m_block_final_state == HAS_BLOCK_FINAL_TX) {
        final_tx = m_chainstate.CoinsTip().GetFinalTx();
        if (final_tx.IsNull()) {
            // Should never happen
            return nullptr;
        }
        // Fetch the unspent outputs of the last block-final tx.  This call
        // should always return results because the prior block-final
        // transaction was the last processed transaction (so none of the
        // outputs could have been spent) or a previously immature coinbase.
        for (uint32_t n = 0; n < final_tx.size; ++n) {
            COutPoint prevout(final_tx.hash, n);
            const auto& coin = m_chainstate.CoinsTip().AccessCoin(prevout);
            if (coin.IsSpent()) {
                // Should never happen
                return nullptr;
            }
            // If it was a coinbase, meaning we're in the first 100 blocks after
            // activation, then we need to make sure it has matured, otherwise
            // we do nothing at all.
            if (coin.IsCoinBase() && (nHeight - coin.nHeight < COINBASE_MATURITY)) {
                // Still maturing. Nothing to do.
                m_block_final_state = NO_BLOCK_FINAL_TX;
                break;
            }
        }
    }

    if (m_block_final_state == HAS_BLOCK_FINAL_TX)
        initFinalTx(final_tx);

    if (m_mempool) {
        LOCK(m_mempool->cs);
        m_mempool->StartBlockBuilding();
        addChunks();
        m_mempool->StopBlockBuilding();
    }

    const auto time_1{SteadyClock::now()};

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;

    // Construct coinbase transaction struct in parallel
    CoinbaseTx& coinbase_tx{pblocktemplate->m_coinbase_tx};
    coinbase_tx.version = coinbaseTx.version;

    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vin[0].nSequence = CTxIn::MAX_SEQUENCE_NONFINAL; // Make sure timelock is enforced.
    coinbase_tx.sequence = coinbaseTx.vin[0].nSequence;

    // Add an output that spends the full coinbase reward.
    coinbaseTx.vout.resize(1);
    coinbaseTx.vout[0].scriptPubKey = m_options.coinbase_output_script;
    coinbaseTx.vout[0].SetReferenceValue(nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus()));
    if (m_block_final_state == INITIAL_BLOCK_FINAL_TXOUT) {
        CTxOut txout(0, CScript() << OP_TRUE);
        coinbaseTx.vout.insert(coinbaseTx.vout.begin(), txout);
    }
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    // Consensus rule: lock-time of coinbase MUST be median-time-past
    coinbaseTx.nLockTime = static_cast<uint32_t>(m_median_time_past);
    coinbaseTx.lock_height = nHeight;
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
    if (m_block_final_state == HAS_BLOCK_FINAL_TX) {
        m_chainstate.m_chainman.GenerateCoinbaseCommitment(*pblock, pindexPrev);
    }
    pblocktemplate->vTxFees[0] = -nFees;

    // The miner needs to know whether the last transaction is a special
    // transaction, or not.
    pblocktemplate->has_block_final_tx = (m_block_final_state == HAS_BLOCK_FINAL_TX);
    for (const auto& item : m_block_final_tx_coin_map) {
        pblocktemplate->block_final_tx_coin_map[item.first] = item.second;
    }

    LogPrintf("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, (m_block_final_state == HAS_BLOCK_FINAL_TX) ? nFees - pblocktemplate->vTxFees.back() : nFees, nBlockSigOpsCost);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    pblock->nNonce         = 0;

    // Use of auxiliary proof-of-work is required after merge mining has
    // activated.  Since interfacing with the auxiliary chain is outside of
    // scope for this code, we generate a minimal auxiliary header.
    if (DeploymentActiveAfter(pindexPrev, m_chainstate.m_chainman, Consensus::DEPLOYMENT_AUXPOW)) {
        // Setup the block header commitment.
        pblock->m_aux_pow.m_commit_version = pblock->nVersion;
        pblock->m_aux_pow.m_commit_hash_merkle_root = BlockTemplateMerkleRoot(*pblock, nullptr);

        // Setup a fake auxiliary block with a single "transaction" (not an
        // actual transaction as no valid data precedes the midstate).
        CSHA256().Midstate(pblock->m_aux_pow.m_midstate_hash.begin(), nullptr, nullptr);
        pblock->m_aux_pow.m_aux_num_txns = 1;

        // Set difficulty for the auxiliary proof-of-work.
        pblock->SetFilteredTime(GetFilteredTimeAux(pindexPrev, chainparams.GetConsensus()));
        pblock->m_aux_pow.m_commit_bits = GetNextWorkRequiredAux(pindexPrev, *pblock, chainparams.GetConsensus());;

        // Setup the auxiliary header fields to have reasonable values.
        pblock->m_aux_pow.m_aux_version = VERSIONBITS_TOP_BITS;
        pblock->m_aux_pow.m_aux_bits = pblock->m_aux_pow.m_commit_bits;
    }

    if (m_options.test_block_validity) {
        // if nHeight <= 16, and include_dummy_extranonce=false this will fail due to bad-cb-length.
        if (BlockValidationState state{TestBlockValidity(m_chainstate, *pblock, /*check_pow=*/false, /*check_merkle_root=*/false)}; !state.IsValid()) {
            throw std::runtime_error(strprintf("TestBlockValidity failed: %s", state.ToString()));
        }
    }
    const auto time_2{SteadyClock::now()};

    LogDebug(BCLog::BENCH, "CreateNewBlock() chunks: %.2fms, validity: %.2fms (total %.2fms)\n",
             Ticks<MillisecondsDouble>(time_1 - time_start),
             Ticks<MillisecondsDouble>(time_2 - time_1),
             Ticks<MillisecondsDouble>(time_2 - time_start));

    return std::move(pblocktemplate);
}

bool BlockAssembler::TestChunkBlockLimits(FeePerWeight chunk_feerate, int64_t chunk_sigops_cost) const
{
    if (nBlockWeight + chunk_feerate.size >= m_options.nBlockMaxWeight) {
        return false;
    }
    if (nBlockSigOpsCost + chunk_sigops_cost >= MAX_BLOCK_SIGOPS_COST) {
        return false;
    }
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
bool BlockAssembler::TestChunkTransactions(const std::vector<CTxMemPoolEntryRef>& txs) const
{
    for (const auto tx : txs) {
        if (!IsFinalTx(tx.get().GetTx(), nHeight, m_lock_time_cutoff)) {
            return false;
        }
    }
    return true;
}

void BlockAssembler::AddToBlock(const CTxMemPoolEntry& entry)
{
    // If we have a block-final transaction, insert just
    // before the end, so the block-final tx remains last.
    pblocktemplate->block.vtx.insert(pblocktemplate->block.vtx.end() - !!(m_block_final_state == HAS_BLOCK_FINAL_TX), iter->GetSharedTx());
    pblocktemplate->vTxFees.insert(pblocktemplate->vTxFees.end() - !!(m_block_final_state == HAS_BLOCK_FINAL_TX), iter->GetFee());
    pblocktemplate->vTxSigOpsCost.insert(pblocktemplate->vTxSigOpsCost.end() - !!(m_block_final_state == HAS_BLOCK_FINAL_TX), iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += GetTimeAdjustedValue(iter->GetFee(), nHeight - iter->GetReferenceHeight());
    inBlock.insert(iter->GetSharedTx()->GetHash());

    if (m_options.print_modified_fee) {
        LogInfo("fee rate %s txid %s\n",
                  CFeeRate(entry.GetModifiedFee(), entry.GetTxSize()).ToString(),
                  entry.GetTx().GetHash().ToString());
    }
}

void BlockAssembler::addChunks()
{
    AssertLockHeld(mempool.cs);

    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc)) {
                continue;
            }
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                mit = mapModifiedTx.insert(modEntry).first;
            }
            mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
        }
    }
    return nDescendantsUpdated;
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

void BlockAssembler::initFinalTx(const BlockFinalTxEntry& final_tx)
{
    // Block-final transactions are only created after we have reached the final
    // state of activation.
    if (m_block_final_state != HAS_BLOCK_FINAL_TX) {
        return;
    }

    LOCK(cs_main); // for m_chainstate.CoinsTip()
    CCoinsViewCache &coins_view = m_chainstate.CoinsTip();

    // Create block-final tx
    CMutableTransaction txFinal;
    txFinal.version = 2;
    txFinal.vout.resize(1);
    txFinal.vout[0].SetReferenceValue(0);
    txFinal.vout[0].scriptPubKey = EMPTY_SEGWIT_COMMITMENT;
    txFinal.nLockTime = static_cast<uint32_t>(m_median_time_past);
    txFinal.lock_height = nHeight;

    // Add all outputs from the prior block-final transaction.  We do nothing
    // here to prevent selected transactions from spending these same outputs
    // out from underneath us; we depend insted on mempool protections that
    // prevent such transactions from being considered in the first place.
    m_block_final_tx_coin_map.clear();
    for (uint32_t n = 0; n < final_tx.size; ++n) {
        COutPoint prevout(final_tx.hash, n);
        const Coin& coin = coins_view.AccessCoin(prevout);
        if (IsTriviallySpendable(coin, prevout, MANDATORY_SCRIPT_VERIFY_FLAGS|SCRIPT_VERIFY_WITNESS|SCRIPT_VERIFY_CLEANSTACK)) {
            m_block_final_tx_coin_map[prevout] = coin;
            txFinal.vin.push_back(CTxIn(prevout, CScript(), CTxIn::SEQUENCE_FINAL));
        } else {
            LogPrintf("WARNING: non-trivial output in block-final transaction record; this should never happen (%s:%n)\n", prevout.hash.ToString(), prevout.n);
        }
    }

    // We should have input(s) for the block-final transaction from the prior
    // block-final transaction, so this should never happen...
    if (txFinal.vin.empty()) {
        LogPrintf("Unable to create block-final transaction due to lack of inputs.\n");
        return;
    }

    // Add block-final transaction to block template.
    pblocktemplate->block.vtx.emplace_back(MakeTransactionRef(std::move(txFinal)));

    // Record the fees forwarded by the block-final transaction to the coinbase.
    CAmount nTxFees = coins_view.GetValueIn(*pblocktemplate->block.vtx.back())
                    - pblocktemplate->block.vtx.back()->GetValueOut();
    nTxFees = GetTimeAdjustedValue(nTxFees, nHeight - txFinal.lock_height);
    pblocktemplate->vTxFees.push_back(nTxFees);
    nFees += nTxFees;

    // The block-final transaction contributes to aggregate limits:
    // the number of sigops is tracked...
    int64_t nTxSigOpsCost = GetTransactionSigOpCost(*pblocktemplate->block.vtx.back(), coins_view, STANDARD_SCRIPT_VERIFY_FLAGS);
    pblocktemplate->vTxSigOpsCost.push_back(nTxSigOpsCost);
    nBlockSigOpsCost += nTxSigOpsCost;

    // ...the size is not:
    nBlockWeight += GetTransactionWeight(*pblocktemplate->block.vtx.back());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(int& nPackagesSelected, int& nDescendantsUpdated)
{
    const auto& mempool{*Assert(m_mempool)};
    LOCK(mempool.cs);

    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    std::set<Txid> failedTx;

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    constexpr int32_t BLOCK_FULL_ENOUGH_WEIGHT_DELTA = 4000;
    int64_t nConsecutiveFailed = 0;

    std::vector<CTxMemPoolEntry::CTxMemPoolEntryRef> selected_transactions;
    selected_transactions.reserve(MAX_CLUSTER_COUNT_LIMIT);
    FeePerWeight chunk_feerate;

    // This fills selected_transactions
    chunk_feerate = m_mempool->GetBlockBuilderChunk(selected_transactions);
    FeePerVSize chunk_feerate_vsize = ToFeePerVSize(chunk_feerate);

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                    CompareTxMemPoolEntryByAncestorFee()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter->GetSharedTx()->GetHash()));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }
        // Ignore demurrage calculations if the refheight age is less than
        // 1008 blocks (1.5 weeks), to speed up block template construction.
        // This heuristic has an error of less than 0.1%.
        if ((iter->GetReferenceHeight() + 1008) < nHeight) {
            packageFees = GetTimeAdjustedValue(packageFees, nHeight - iter->GetReferenceHeight());
        }

        if (packageFees < m_options.blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        int64_t chunk_sig_ops = 0;
        for (const auto& tx : selected_transactions) {
            chunk_sig_ops += tx.get().GetSigOpCost();
        }

        // Check to see if this chunk will fit.
        if (!TestChunkBlockLimits(chunk_feerate, chunk_sig_ops) || !TestChunkTransactions(selected_transactions)) {
            // This chunk won't fit, so we skip it and will try the next best one.
            m_mempool->SkipBuilderChunk();
            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight +
                    BLOCK_FULL_ENOUGH_WEIGHT_DELTA > m_options.nBlockMaxWeight) {
                // Give up if we're close to full and haven't succeeded in a while
                return;
            }
        } else {
            m_mempool->IncludeBuilderChunk();

            // This chunk will fit, so add it to the block.
            nConsecutiveFailed = 0;
            for (const auto& tx : selected_transactions) {
                AddToBlock(tx);
            }
            pblocktemplate->m_package_feerates.emplace_back(chunk_feerate_vsize);
        }

        selected_transactions.clear();
        chunk_feerate = m_mempool->GetBlockBuilderChunk(selected_transactions);
        chunk_feerate_vsize = ToFeePerVSize(chunk_feerate);
    }
}

void AddMerkleRootAndCoinbase(CBlock& block, CTransactionRef coinbase, uint32_t version, uint32_t timestamp, uint32_t nonce)
{
    if (block.vtx.size() == 0) {
        block.vtx.emplace_back(coinbase);
    } else {
        block.vtx[0] = coinbase;
    }
    block.nVersion = version;
    block.nTime = timestamp;
    block.nNonce = nonce;
    block.hashMerkleRoot = BlockMerkleRoot(block);

    // Reset cached checks
    block.m_checked_witness_commitment = false;
    block.m_checked_merkle_root = false;
    block.fChecked = false;
}

void InterruptWait(KernelNotifications& kernel_notifications, bool& interrupt_wait)
{
    LOCK(kernel_notifications.m_tip_block_mutex);
    interrupt_wait = true;
    kernel_notifications.m_tip_block_cv.notify_all();
}

std::unique_ptr<CBlockTemplate> WaitAndCreateNewBlock(ChainstateManager& chainman,
                                                      KernelNotifications& kernel_notifications,
                                                      CTxMemPool* mempool,
                                                      const std::unique_ptr<CBlockTemplate>& block_template,
                                                      const BlockWaitOptions& options,
                                                      const BlockAssembler::Options& assemble_options,
                                                      bool& interrupt_wait)
{
    // Delay calculating the current template fees, just in case a new block
    // comes in before the next tick.
    CAmount current_fees = -1;

    // Alternate waiting for a new tip and checking if fees have risen.
    // The latter check is expensive so we only run it once per second.
    auto now{NodeClock::now()};
    const auto deadline = now + options.timeout;
    const MillisecondsDouble tick{1000};
    const bool allow_min_difficulty{chainman.GetParams().GetConsensus().fPowAllowMinDifficultyBlocks};

    do {
        bool tip_changed{false};
        {
            WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
            // Note that wait_until() checks the predicate before waiting
            kernel_notifications.m_tip_block_cv.wait_until(lock, std::min(now + tick, deadline), [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
                AssertLockHeld(kernel_notifications.m_tip_block_mutex);
                const auto tip_block{kernel_notifications.TipBlock()};
                // We assume tip_block is set, because this is an instance
                // method on BlockTemplate and no template could have been
                // generated before a tip exists.
                tip_changed = Assume(tip_block) && tip_block != block_template->block.hashPrevBlock;
                return tip_changed || chainman.m_interrupt || interrupt_wait;
            });
            if (interrupt_wait) {
                interrupt_wait = false;
                return nullptr;
            }
        }

        if (chainman.m_interrupt) return nullptr;
        // At this point the tip changed, a full tick went by or we reached
        // the deadline.

        // Must release m_tip_block_mutex before locking cs_main, to avoid deadlocks.
        LOCK(::cs_main);

        // On test networks return a minimum difficulty block after 20 minutes
        if (!tip_changed && allow_min_difficulty) {
            const NodeClock::time_point tip_time{std::chrono::seconds{chainman.ActiveChain().Tip()->GetBlockTime()}};
            if (now > tip_time + 20min) {
                tip_changed = true;
            }
        }

        /**
         * We determine if fees increased compared to the previous template by generating
         * a fresh template. There may be more efficient ways to determine how much
         * (approximate) fees for the next block increased, perhaps more so after
         * Cluster Mempool.
         *
         * We'll also create a new template if the tip changed during this iteration.
         */
        if (options.fee_threshold < MAX_MONEY || tip_changed) {
            auto new_tmpl{BlockAssembler{
                chainman.ActiveChainstate(),
                mempool,
                assemble_options}
                              .CreateNewBlock()};

            // If the tip changed, return the new template regardless of its fees.
            if (tip_changed) return new_tmpl;

            // Calculate the original template total fees if we haven't already
            if (current_fees == -1) {
                current_fees = std::accumulate(block_template->vTxFees.begin(), block_template->vTxFees.end(), CAmount{0});
            }

            // Check if fees increased enough to return the new template
            const CAmount new_fees = std::accumulate(new_tmpl->vTxFees.begin(), new_tmpl->vTxFees.end(), CAmount{0});
            Assume(options.fee_threshold != MAX_MONEY);
            if (new_fees >= current_fees + options.fee_threshold) return new_tmpl;
        }

        now = NodeClock::now();
    } while (now < deadline);

    return nullptr;
}

std::optional<BlockRef> GetTip(ChainstateManager& chainman)
{
    LOCK(::cs_main);
    CBlockIndex* tip{chainman.ActiveChain().Tip()};
    if (!tip) return {};
    return BlockRef{tip->GetBlockHash(), tip->nHeight};
}

bool CooldownIfHeadersAhead(ChainstateManager& chainman, KernelNotifications& kernel_notifications, const BlockRef& last_tip, bool& interrupt_mining)
{
    uint256 last_tip_hash{last_tip.hash};

    while (const std::optional<int> remaining = chainman.BlocksAheadOfTip()) {
        const int cooldown_seconds = std::clamp(*remaining, 3, 20);
        const auto cooldown_deadline{MockableSteadyClock::now() + std::chrono::seconds{cooldown_seconds}};

        {
            WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
            kernel_notifications.m_tip_block_cv.wait_until(lock, cooldown_deadline, [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
                const auto tip_block = kernel_notifications.TipBlock();
                return chainman.m_interrupt || interrupt_mining || (tip_block && *tip_block != last_tip_hash);
            });
            if (chainman.m_interrupt || interrupt_mining) {
                interrupt_mining = false;
                return false;
            }

            // If the tip changed during the wait, extend the deadline
            const auto tip_block = kernel_notifications.TipBlock();
            if (tip_block && *tip_block != last_tip_hash) {
                last_tip_hash = *tip_block;
                continue;
            }
        }

        // No tip change and the cooldown window has expired.
        if (MockableSteadyClock::now() >= cooldown_deadline) break;
    }

    return true;
}

std::optional<BlockRef> WaitTipChanged(ChainstateManager& chainman, KernelNotifications& kernel_notifications, const uint256& current_tip, MillisecondsDouble& timeout, bool& interrupt)
{
    Assume(timeout >= 0ms); // No internal callers should use a negative timeout
    if (timeout < 0ms) timeout = 0ms;
    if (timeout > std::chrono::years{100}) timeout = std::chrono::years{100}; // Upper bound to avoid UB in std::chrono
    auto deadline{std::chrono::steady_clock::now() + timeout};
    {
        WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
        // For callers convenience, wait longer than the provided timeout
        // during startup for the tip to be non-null. That way this function
        // always returns valid tip information when possible and only
        // returns null when shutting down, not when timing out.
        kernel_notifications.m_tip_block_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
            return kernel_notifications.TipBlock() || chainman.m_interrupt || interrupt;
        });
        if (chainman.m_interrupt || interrupt) {
            interrupt = false;
            return {};
        }
        // At this point TipBlock is set, so continue to wait until it is
        // different then `current_tip` provided by caller.
        kernel_notifications.m_tip_block_cv.wait_until(lock, deadline, [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
            return Assume(kernel_notifications.TipBlock()) != current_tip || chainman.m_interrupt || interrupt;
        });
        if (chainman.m_interrupt || interrupt) {
            interrupt = false;
            return {};
        }
    }

    // Must release m_tip_block_mutex before getTip() locks cs_main, to
    // avoid deadlocks.
    return GetTip(chainman);
}

} // namespace node

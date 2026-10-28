// Copyright (c) 2020-2023 The Freicoin Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sharechain.h>

#include <chainparams.h> // for Params()

#include <consensus/merkle.h> // for ComputeMerkleMapRootFromBranch, ComputeMerkleRootFromBranch
#include <hash.h> // for CHash256, HashWriter
#include <primitives/block.h> // for CBlockHeader
#include <rpc/server.h> // for CRPCTable
#include <rpc/server_util.h> // for EnsureAnyNodeContext
#include <node/context.h> // for NodeContext
#include <util/strencodings.h> // for strprintf via tinyformat
#include <tinyformat.h>
#include <streams.h> // for DataStream
#include <span.h> // for Span, MakeUCharSpan
#include <util/check.h> // for Assert

#include <algorithm> // for std::sort
#include <set> // for std::set
#include <functional> // for std::function
#include <chrono> // for std::chrono
#include <util/fs_helpers.h> // for FileCommit, RenameOver
#include <logging.h> // for LogPrintf
#include <array> // for std::array
#include <utility> // for std::swap

void SetupShareChainParamsOptions(ArgsManager& argsman) {
    argsman.AddArg("-sharechain=<name>", "Use the share chain <name> (default: main). Allowed values: solo, main or regtest", ArgsManager::ALLOW_ANY, OptionsCategory::STRATUM);
}

struct SoloShareChainParams : public ShareChainParams {
    SoloShareChainParams() {
        is_valid = false;
        m_share_chain_type = ShareChainType::SOLO;
    }
};

struct MainShareChainParams : public ShareChainParams {
    MainShareChainParams() {
        is_valid = true;
        m_share_chain_type = ShareChainType::MAIN;
        m_chain_id = uint256S("5348415245434841494e2d4d41494e0000000000000000000000000000000002"); // 'SHARECHAIN-MAIN' || 2 (2: aux share identity = parent header hash)
        m_share_bits = 0x1b068db2;   // difficulty 10000: ~43 TH for 10 s shares; also the easiest target allowed
        m_retarget = true;
        m_share_interval = 10;
    }
};

struct RegtestShareChainParams : public ShareChainParams {
    RegtestShareChainParams() {
        is_valid = true;
        m_share_chain_type = ShareChainType::REGTEST;
        m_share_bits = 0x207fffff;   // every hash is a share: tests and the regtest stack
        m_chain_factor = 4;
        m_recency = 10;
        m_retarget = false;
        m_chain_id = uint256S("5348415245434841494e2d5245475445535400000000000000000000000002");
    }
};

std::unique_ptr<const ShareChainParams> g_share_chain_params;

void SelectShareParams(const ShareChainType share_chain) {
    switch (share_chain) {
    case ShareChainType::SOLO:
        g_share_chain_params = std::unique_ptr<const ShareChainParams>(new SoloShareChainParams());
        return;
    case ShareChainType::MAIN:
        g_share_chain_params = std::unique_ptr<const ShareChainParams>(new MainShareChainParams());
        return;
    case ShareChainType::REGTEST:
        g_share_chain_params = std::unique_ptr<const ShareChainParams>(new RegtestShareChainParams());
        return;
    }
    // Switch statement is exhaustive.
    assert(false);
}

const ShareChainParams& ShareParams() {
    Assert(g_share_chain_params);
    return *g_share_chain_params;
}

void swap(ShareWitness& lhs, ShareWitness& rhs) {
    using std::swap; // for ADL
    swap(lhs.flags, rhs.flags);
    swap(lhs.aux, rhs.aux);
    swap(lhs.commit, rhs.commit);
    swap(lhs.cb1, rhs.cb1);
    swap(lhs.nLockTime, rhs.nLockTime);
    swap(lhs.lock_height, rhs.lock_height);
    swap(lhs.branch, rhs.branch);
    swap(lhs.nVersion, rhs.nVersion);
    swap(lhs.hashPrevBlock, rhs.hashPrevBlock);
    swap(lhs.share_chain_path, rhs.share_chain_path);
    swap(lhs.nTime, rhs.nTime);
    swap(lhs.nBits, rhs.nBits);
    swap(lhs.nNonce, rhs.nNonce);
}

void swap(Share& lhs, Share& rhs) {
    using std::swap; // for ADL
    swap(lhs.version, rhs.version);
    swap(lhs.bits, rhs.bits);
    swap(lhs.height, rhs.height);
    swap(lhs.total_work, rhs.total_work);
    swap(lhs.anchor, rhs.anchor);
    swap(lhs.prev_root, rhs.prev_root);
    swap(lhs.fruits_root, rhs.fruits_root);
    swap(lhs.miner, rhs.miner);
    swap(lhs.wit, rhs.wit);
}

uint256 Share::GetHeaderHash() const {
    HashWriter ss{};
    ss << version;
    ss << bits;
    ss << height;
    ss << total_work;
    ss << anchor;
    ss << prev_root;
    ss << fruits_root;
    ss << COMPACTSIZE(miner.GetWitnessVersion()); // will always be single byte
    ss << miner.GetWitnessProgram();
    return ss.GetHash();
}

CBlockHeader Share::GetBlockHeader(bool *mutated) const {
    if (mutated) {
        *mutated = false;
    }

    // Compute the hash of the share header.
    uint256 hash = GetHeaderHash();

    // Compute the commitment root hash.
    // The share chain commitment might be stored alongside other
    // commitments in the form of a Merkle hash map.  We therefore use
    // the branch proof to work our way up to the root value of the
    // Merkle hash map.
    bool invalid = false;
    hash = ComputeMerkleMapRootFromBranch(hash, wit.commit, wit.share_chain_path, &invalid);
    if (invalid && mutated) {
        *mutated = true;
    }

    // Calculate hash of coinbase transaction.
    CHash256 h;
    // Write the first part of the coinbase transaction.
    h.Write(MakeUCharSpan(wit.cb1));
    // Write the commitment root hash.
    h.Write(MakeUCharSpan(hash));
    // Write the commitment identifier.
    static const std::array<unsigned char, 4> id
        = { 0x4b, 0x4a, 0x49, 0x48 };
    h.Write(MakeUCharSpan(id));
    // Write the rest of the coinbase transaction.
    {
        DataStream ds{};
        ds << wit.nLockTime;
        ds << wit.lock_height;
        h.Write(MakeUCharSpan(ds));
    }
    h.Finalize(hash);

    // Calculate hashMerkleRoot for the block header.
    hash = ComputeMerkleRootFromBranch(hash, wit.branch, 0);

    // Write the block header fields.
    CBlockHeader blkhdr;
    blkhdr.nVersion = wit.nVersion;
    blkhdr.hashPrevBlock = wit.hashPrevBlock;
    blkhdr.hashMerkleRoot = hash;
    blkhdr.nTime = wit.nTime;
    blkhdr.nBits = wit.nBits;
    blkhdr.nNonce = wit.nNonce;
    if (IsAux()) {
        blkhdr.m_aux_pow = *wit.aux;
    }
    return blkhdr;
}

uint256 Share::GetPoWHash(const Consensus::Params& params, bool* mutated) const
{
    bool local_mutated = false;
    const CBlockHeader blkhdr = GetBlockHeader(&local_mutated);
    if (!IsAux()) {
        if (mutated) *mutated = local_mutated;
        return blkhdr.GetHash();
    }
    bool aux_mutated = false;
    const uint256 hash = blkhdr.GetAuxiliaryHash(params, &aux_mutated).first;
    if (mutated) *mutated = local_mutated || aux_mutated;
    return hash;
}

uint256 Share::GetHash() const
{
    if (IsAux()) {
        // The Freicoin header of one job is the same for every parent hash the
        // miner finds; the parent header hash is what makes a share unique
        // (seen live: every submission after the first in a job was a duplicate).
        return GetPoWHash(::Params().GetConsensus(), nullptr);
    }
    return GetBlockHeader(nullptr).GetHash();
}

void SetAuxShareWitness(Share& share, const AuxProofOfWork& aux_pow)
{
    share.wit.flags |= ShareWitness::FLAG_AUX;
    share.wit.aux = aux_pow;
}

arith_uint256 GetShareWork(uint32_t bits)
{
    arith_uint256 target;
    bool negative = false, overflow = false;
    target.SetCompact(bits, &negative, &overflow);
    if (negative || overflow || target == 0) {
        return 0;
    }
    // 2**256 / (target+1) == ~target / (target+1) + 1, see GetBlockProof.
    return (~target / (target + 1)) + 1;
}

ShareCredits ComputeShareCredits(const ShareIndex* anchor, const arith_uint256& window_work, const FruitLookup& lookup)
{
    ShareCredits credits;
    arith_uint256 acc = 0;
    for (const ShareIndex* cur = anchor; cur && acc < window_work; cur = cur->prev_share.get()) {
        for (const uint256& fruit : cur->fruits) {
            std::optional<FruitInfo> info = lookup(fruit);
            if (!info) {
                continue;
            }
            const arith_uint256 work = GetShareWork(info->bits);
            credits[info->miner] += work;
            acc += work;
        }
    }
    return credits;
}

namespace {
// out_m = floor(total * credit_m / sum), remainder to the builder.
std::map<WitnessUnknown, CAmount> SplitPayout(const ShareCredits& credits, CAmount total, const WitnessUnknown& builder)
{
    arith_uint256 sum = 0;
    for (const auto& item : credits) {
        sum += item.second;
    }
    std::map<WitnessUnknown, CAmount> outs;
    CAmount paid = 0;
    if (sum != 0) {
        const arith_uint256 total256(static_cast<uint64_t>(total));
        for (const auto& item : credits) {
            const CAmount amount = static_cast<CAmount>((total256 * item.second / sum).GetLow64());
            outs[item.first] = amount;
            paid += amount;
        }
    }
    const CAmount remainder = total - paid;
    if (remainder > 0 || outs.count(builder)) {
        outs[builder] += remainder;
    }
    return outs;
}
} // namespace

std::vector<std::pair<WitnessUnknown, CAmount>> ComputeSharePayouts(ShareCredits credits, CAmount total, const WitnessUnknown& builder, CAmount dust, size_t max_payees)
{
    // Scale so that total * credit cannot overflow 256 bits: keep the sum
    // below 2^190 (total is below 2^63).
    arith_uint256 sum = 0;
    for (const auto& item : credits) {
        sum += item.second;
    }
    const unsigned int bits = sum.bits();
    if (bits > 190) {
        const unsigned int shift = bits - 190;
        for (auto& item : credits) {
            item.second >>= shift;
        }
    }

    std::map<WitnessUnknown, CAmount> outs = SplitPayout(credits, total, builder);

    // Drop dust (never the builder's output) and recompute once.
    bool dropped = false;
    for (const auto& item : outs) {
        if (item.second < dust && !(item.first == builder)) {
            credits.erase(item.first);
            dropped = true;
        }
    }
    if (dropped) {
        outs = SplitPayout(credits, total, builder);
    }

    // Cap the number of payees: keep the largest credits (ties by canonical
    // order) and recompute once.
    if (outs.size() > max_payees) {
        std::vector<std::pair<WitnessUnknown, arith_uint256>> ranked(credits.begin(), credits.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
        ShareCredits kept;
        for (size_t i = 0; i < ranked.size() && i < max_payees; ++i) {
            kept.insert(ranked[i]);
        }
        credits = std::move(kept);
        outs = SplitPayout(credits, total, builder);
    }

    return std::vector<std::pair<WitnessUnknown, CAmount>>(outs.begin(), outs.end());
}

// ---------------------------------------------------------------------------
// Share chain state

static const std::array<unsigned char, 4> SHARE_COMMIT_ID = { 0x4b, 0x4a, 0x49, 0x48 };

uint256 PrevRootAfter(const ShareIndex* anchor)
{
    if (!anchor) {
        return MmrAccumulator().GetHash();
    }
    MmrAccumulator mmr(anchor->peaks);
    mmr.Append(anchor->GetHash());
    return mmr.GetHash();
}

uint256 FruitsRoot(const std::vector<uint256>& fruits)
{
    if (fruits.empty()) {
        return uint256();
    }
    return ComputeMerkleRoot(fruits, nullptr);
}

CScript ShareCommitmentScript(const uint256& commitment_root)
{
    std::vector<unsigned char> data(commitment_root.begin(), commitment_root.end());
    data.insert(data.end(), SHARE_COMMIT_ID.begin(), SHARE_COMMIT_ID.end());
    return CScript() << OP_RETURN << data;
}

bool ReconstructShareCoinbase(const Share& share, CMutableTransaction& cb)
{
    // The witness stores the coinbase up to the commitment root; the rest is
    // root || id || nLockTime || lock_height, exactly as GetBlockHeader hashes it.
    bool invalid = false;
    const uint256 root = ComputeMerkleMapRootFromBranch(share.GetHeaderHash(), share.wit.commit, share.wit.share_chain_path, &invalid);
    if (invalid) {
        return false;
    }
    DataStream ds{};
    ds.write(MakeByteSpan(share.wit.cb1));
    ds << root;
    ds.write(MakeByteSpan(SHARE_COMMIT_ID));
    ds << share.wit.nLockTime;
    ds << share.wit.lock_height;
    try {
        ds >> TX_NO_WITNESS(cb);
    } catch (const std::exception&) {
        return false;
    }
    return ds.empty();
}

arith_uint256 ShareChainstateManager::ShareTarget() const
{
    arith_uint256 target;
    target.SetCompact(Params().ShareBits());
    return target;
}

arith_uint256 ShareChainstateManager::ChainTarget() const
{
    return ShareTarget() / std::max<uint32_t>(1, Params().ChainFactor());
}

arith_uint256 ShareChainstateManager::ShareTargetFor(uint32_t bits) const
{
    arith_uint256 target;
    bool negative = false, overflow = false;
    target.SetCompact(bits, &negative, &overflow);
    if (negative || overflow || target == 0) {
        return 0;
    }
    return target;
}

arith_uint256 ShareChainstateManager::ChainTargetFor(uint32_t bits) const
{
    return ShareTargetFor(bits) / std::max<uint32_t>(1, Params().ChainFactor());
}

void ShareChainstateManager::Initialize()
{
    if (!m_active_chainstate) {
        m_active_chainstate = std::make_shared<ShareChainstate>();
    }
}

std::optional<FruitInfo> ShareChainstateManager::LookupFruit(const uint256& hash) const
{
    const auto& fruits = ActiveChainstate().fruits;
    auto it = fruits.find(hash);
    if (it == fruits.end()) {
        return std::nullopt;
    }
    return FruitInfo{it->second.miner, it->second.bits};
}

bool ShareChainstateManager::CheckLinkage(const Share& share, const ShareIndex** anchor_out, std::string& reason) const
{
    const ShareChainstate& state = ActiveChainstate();
    const ShareIndex* anchor = nullptr;
    if (!share.anchor.IsNull()) {
        auto it = state.chain_shares.find(share.anchor);
        if (it == state.chain_shares.end()) {
            reason = "share-anchor-unknown";
            return false;
        }
        anchor = it->second.get();
    } else if (ActiveHeight() >= static_cast<int>(Params().Recency())) {
        // Fruits of the empty-chain template keep arriving after the genesis chain
        // share (the miners are still on that job) and the next chain shares list
        // them; they are legitimate, and listable, while the chain is younger than R.
        // (Seen live: a reload rejected them and nothing above resolved.)
        reason = "share-anchor-null";
        return false;
    }
    const uint32_t expected_height = anchor ? anchor->share.height + 1 : 0;
    if (share.height != expected_height) {
        reason = "share-bad-height";
        return false;
    }
    arith_uint256 expected_work = 0;
    if (anchor) {
        expected_work = UintToArith256(anchor->share.total_work) + GetShareWork(anchor->share.bits);
    }
    if (UintToArith256(share.total_work) != expected_work) {
        reason = "share-bad-total-work";
        return false;
    }
    if (share.prev_root != PrevRootAfter(anchor)) {
        reason = "share-bad-prev-root";
        return false;
    }
    if (share.bits != RequiredShareBits(anchor)) {
        reason = "share-bad-bits";
        return false;
    }
    if (share.wit.share_chain_path != Params().ChainId()) {
        reason = "share-bad-chain-id";
        return false;
    }
    if (anchor_out) {
        *anchor_out = anchor;
    }
    return true;
}

namespace {
/** Whether the PPLNS window walked back from `anchor` runs into a trusted base
 *  (a chain share whose own anchor was never seen) before it is full: the
 *  credits below are history this node does not have. */
bool WindowReachesBase(const ShareIndex* anchor, const arith_uint256& window, const FruitLookup& lookup)
{
    arith_uint256 acc = 0;
    for (const ShareIndex* cur = anchor; cur && acc < window; cur = cur->prev_share.get()) {
        for (const uint256& f : cur->fruits) {
            const std::optional<FruitInfo> info = lookup(f);
            if (info) acc += GetShareWork(info->bits);
        }
        if (acc < window && !cur->prev_share && !cur->share.anchor.IsNull()) {
            return true;
        }
    }
    return false;
}
} // namespace

bool ShareChainstateManager::CheckPayouts(const Share& share, const ShareIndex* anchor, std::string& reason) const
{
    CMutableTransaction cb;
    if (!ReconstructShareCoinbase(share, cb)) {
        reason = "share-bad-coinbase";
        return false;
    }
    if (cb.vout.empty()) {
        reason = "share-coinbase-no-outputs";
        return false;
    }
    // The last output is the commitment; everything before it is the payout vector.
    CAmount total = 0;
    for (const CTxOut& out : cb.vout) {
        total += out.GetReferenceValue();
    }
    // The window is sized by the Freicoin target the template was built for;
    // the share header has no such field, so the coinbase's own bits are not
    // available here: use the share chain's notion (window in shares) via the
    // Freicoin header's nBits carried in the witness.
    const arith_uint256 window = GetShareWork(share.wit.nBits) * Params().WindowK();
    const FruitLookup lookup = [this](const uint256& h) { return LookupFruit(h); };
    if (WindowReachesBase(anchor, window, lookup)) {
        // Below our trusted base: the share was built from credits we never saw, so
        // its split cannot be checked.  It is history; its own payout only matters
        // if it became a Freicoin block, which the block chain validated.
        return true;
    }
    ShareCredits credits = ComputeShareCredits(anchor, window, lookup);
    const auto payouts = ComputeSharePayouts(std::move(credits), total, share.miner, Params().Dust(), Params().MaxPayees());
    if (cb.vout.size() < payouts.size() + 1) {
        reason = "share-bad-payout-count";
        return false;
    }
    for (size_t i = 0; i < payouts.size(); ++i) {
        const CScript expected = GetScriptForDestination(payouts[i].first);
        if (cb.vout[i].GetReferenceValue() != payouts[i].second || cb.vout[i].scriptPubKey != expected) {
            reason = "share-bad-payout";
            return false;
        }
    }
    // Anything between the payouts and the commitment (none today) must carry no value.
    for (size_t i = payouts.size(); i < cb.vout.size(); ++i) {
        if (cb.vout[i].GetReferenceValue() != 0) {
            reason = "share-bad-extra-output";
            return false;
        }
    }
    // The commitment output itself is implied: ReconstructShareCoinbase appended the root
    // derived from this header, and AcceptFruit checks the resulting block hash against the PoW.
    return true;
}

bool ShareChainstateManager::CheckAuxBinding(const Share& share, std::string& reason) const
{
    // The parent header hash covers the Freicoin template through the aux-pow
    // commit fields; the commit Merkle root is computed from the coinbase with
    // an empty scriptSig (CustomizeCommitHash).  Recompute it from the witness
    // coinbase so the payouts and the share commitment are what was mined.
    CMutableTransaction cb;
    if (!ReconstructShareCoinbase(share, cb) || cb.vin.empty()) {
        reason = "share-bad-coinbase";
        return false;
    }
    CMutableTransaction cb2(cb);
    cb2.vin[0].scriptSig = CScript();
    cb2.vin[0].nSequence = 0;
    const uint256 commit_root = ComputeMerkleRootFromBranch(cb2.GetHash(), share.wit.branch, 0);
    if (commit_root != share.wit.aux->m_commit_hash_merkle_root) {
        reason = "share-aux-commit-mismatch";
        return false;
    }
    return true;
}

bool ShareChainstateManager::AcceptFruit(const Share& share, std::string& reason, bool live)
{
    ShareChainstate& state = ActiveChainstate();
    const uint256 hash = share.GetHash();
    if (state.fruits.count(hash)) {
        reason = "share-duplicate";
        return false;
    }
    const ShareIndex* anchor = nullptr;
    if (!CheckLinkage(share, &anchor, reason)) {
        return false;
    }
    bool mutated = false;
    const uint256 pow_hash = share.GetPoWHash(m_options.chainparams.GetConsensus(), &mutated);
    if (mutated || UintToArith256(pow_hash) > ShareTargetFor(share.bits)) {
        reason = "share-high-hash";
        return false;
    }
    if (share.IsAux() && !CheckAuxBinding(share, reason)) {
        return false;
    }
    {
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(m_options.adjusted_time_callback().time_since_epoch()).count();
        if (static_cast<int64_t>(share.wit.nTime) > now + 2 * 60 * 60) {
            reason = "share-time-too-new";
            return false;
        }
        if (anchor && static_cast<int64_t>(share.wit.nTime) + 10 * 60 < static_cast<int64_t>(anchor->share.wit.nTime)) {
            reason = "share-time-too-old";
            return false;
        }
        // Template age: the Freicoin block the template builds on must be known, and a
        // live share (just mined or relayed) must build on one of the last
        // MaxTemplateAge() blocks. Shares from a sync batch or sharechain.dat keep only
        // the known-block requirement.
        const int tip_height = m_options.tip_height_callback ? m_options.tip_height_callback() : -1;
        if (tip_height >= 0 && m_options.block_height_callback) {
            const std::optional<int> prev_height = m_options.block_height_callback(share.wit.hashPrevBlock);
            if (!prev_height) {
                reason = "share-unknown-prev-block";
                return false;
            }
            if (live && *prev_height + Params().MaxTemplateAge() < tip_height) {
                reason = "share-stale-template";
                return false;
            }
        }
        // The PPLNS window is sized by the template's nBits (CheckPayouts): it must be
        // the Freicoin difficulty for that block, not whatever the miner wrote.
        if (m_options.required_bits_callback) {
            const std::optional<uint32_t> required = m_options.required_bits_callback(share.wit.hashPrevBlock, share.GetBlockHeader(nullptr));
            if (required && *required != share.wit.nBits) {
                reason = "share-bad-frc-bits";
                return false;
            }
        }
    }
    if (!CheckPayouts(share, anchor, reason)) {
        return false;
    }
    state.fruits.emplace(hash, share);
    return true;
}

std::vector<uint256> ShareChainstateManager::ListableFruits(const ShareIndex* tip) const
{
    const ShareChainstate& state = ActiveChainstate();
    const uint32_t new_height = tip ? tip->share.height + 1 : 0;
    const uint32_t recency = Params().Recency();
    // Fruits listed by the last R chain shares are excluded.
    std::set<uint256> listed;
    {
        const ShareIndex* cur = tip;
        for (uint32_t i = 0; cur && i < recency; ++i, cur = cur->prev_share.get()) {
            listed.insert(cur->fruits.begin(), cur->fruits.end());
        }
    }
    std::vector<uint256> out;
    for (const auto& item : state.fruits) {
        const Share& f = item.second;
        if (listed.count(item.first)) continue;
        // anchor must be on the chain below the new share, within recency
        const ShareIndex* a = nullptr;
        if (!f.anchor.IsNull()) {
            auto it = state.chain_shares.find(f.anchor);
            if (it == state.chain_shares.end()) continue;
            a = it->second.get();
            if (a->share.height + recency < new_height) continue;
            // on the active chain?
            const ShareIndex* cur = tip;
            while (cur && cur->share.height > a->share.height) cur = cur->prev_share.get();
            if (cur != a) continue;
        } else if (tip && new_height > recency) {
            continue;
        }
        out.push_back(item.first);
    }
    std::sort(out.begin(), out.end());
    return out;
}

ShareChainstateManager::TemplateState ShareChainstateManager::GetTemplateState(const WitnessUnknown& miner, CAmount total, uint32_t frc_bits) const
{
    const std::shared_ptr<ShareIndex> tip = ActiveTip();
    TemplateState st;
    st.anchor = tip ? tip->GetHash() : uint256();
    st.prev_root = PrevRootAfter(tip.get());
    st.height = tip ? tip->share.height + 1 : 0;
    st.total_work = tip ? ArithToUint256(UintToArith256(tip->share.total_work) + GetShareWork(tip->share.bits)) : uint256();
    st.bits = RequiredShareBits(tip.get());
    st.fruits = ListableFruits(tip.get());
    st.fruits_root = FruitsRoot(st.fruits);
    if (tip) {
        st.peaks = tip->peaks;
        st.peaks.Append(tip->GetHash());
    }
    const arith_uint256 window = GetShareWork(frc_bits) * Params().WindowK();
    const FruitLookup lookup = [this](const uint256& h) { return LookupFruit(h); };
    st.credits = ComputeShareCredits(tip.get(), window, lookup);
    st.payouts = ComputeSharePayouts(st.credits, total, miner, Params().Dust(), Params().MaxPayees());
    return st;
}

bool ShareChainstateManager::AcceptChainShare(const Share& share, const MmrAccumulator& peaks, const std::vector<uint256>& fruits, std::string& reason, bool live)
{
    ShareChainstate& state = ActiveChainstate();
    const uint256 hash = share.GetHash();
    if (state.chain_shares.count(hash)) {
        reason = "chainshare-duplicate";
        return false;
    }
    // A chain share is a share: record it as a fruit first (idempotent).
    if (!state.fruits.count(hash)) {
        if (!AcceptFruit(share, reason, live)) {
            return false;
        }
    }
    const ShareIndex* parent = nullptr;
    if (!CheckLinkage(share, &parent, reason)) {
        return false;
    }
    bool mutated = false;
    const uint256 pow_hash = share.GetPoWHash(m_options.chainparams.GetConsensus(), &mutated);
    if (mutated || UintToArith256(pow_hash) > ChainTargetFor(share.bits)) {
        reason = "chainshare-high-hash";
        return false;
    }
    // C1: the peaks are the MMR of the chain shares before this one, i.e. the
    // accumulator whose root (after appending the parent) is prev_root.
    MmrAccumulator expected_peaks = parent ? parent->peaks : MmrAccumulator();
    if (parent) {
        expected_peaks.Append(parent->GetHash());
    }
    if (expected_peaks.GetHash() != peaks.GetHash() || peaks.GetHash() != share.prev_root) {
        reason = "chainshare-bad-peaks";
        return false;
    }
    // C2: fruit list sorted, unique, known, anchored within recency, not listed
    // by the last R ancestors.  Just above a trusted base the listed fruits may
    // hang from pruned ancestors we never saw; they are below the window.
    const bool near_base = NearBase(parent, share.height);
    if (!std::is_sorted(fruits.begin(), fruits.end()) || std::adjacent_find(fruits.begin(), fruits.end()) != fruits.end()) {
        reason = "chainshare-fruits-unsorted";
        return false;
    }
    if (FruitsRoot(fruits) != share.fruits_root) {
        reason = "chainshare-bad-fruits-root";
        return false;
    }
    {
        std::set<uint256> listed;
        const ShareIndex* cur = parent;
        for (uint32_t i = 0; cur && i < Params().Recency(); ++i, cur = cur->prev_share.get()) {
            listed.insert(cur->fruits.begin(), cur->fruits.end());
        }
        for (const uint256& f : fruits) {
            auto it = state.fruits.find(f);
            if (it == state.fruits.end()) {
                if (near_base) continue;
                reason = "chainshare-fruit-unknown";
                return false;
            }
            if (listed.count(f)) {
                reason = "chainshare-fruit-relisted";
                return false;
            }
            const Share& fruit = it->second;
            if (!fruit.anchor.IsNull()) {
                auto a = state.chain_shares.find(fruit.anchor);
                if (a == state.chain_shares.end()) {
                    if (near_base) continue;
                    reason = "chainshare-fruit-anchor-unknown";
                    return false;
                }
                if (a->second->share.height + Params().Recency() < share.height) {
                    reason = "chainshare-fruit-stale";
                    return false;
                }
            }
        }
    }
    auto index = std::make_shared<ShareIndex>();
    index->share = share;
    index->peaks = peaks;
    index->fruits = fruits;
    index->base_total_work = parent ? parent->base_total_work : share.total_work;
    if (parent) {
        index->prev_share = state.chain_shares.at(share.anchor);
    }
    state.chain_shares.emplace(hash, index);
    // C3: most verified work wins; ties keep the current tip.
    const std::shared_ptr<ShareIndex> tip = ActiveTip();
    if (!tip || GetVerifiedChainWork(*index) > GetVerifiedChainWork(*tip)) {
        SetActiveTip(index);
        Prune();
    }
    return true;
}

arith_uint256 GetVerifiedChainWork(const ShareIndex& index)
{
    return UintToArith256(index.share.total_work) + GetShareWork(index.share.bits) - UintToArith256(index.base_total_work);
}

bool ShareChainstateManager::NearBase(const ShareIndex* parent, uint32_t height) const
{
    const ShareIndex* base = parent;
    while (base && base->prev_share) base = base->prev_share.get();
    // a genesis chain share (null anchor) is not a trusted base: everything below it exists
    return base && !base->share.anchor.IsNull() && height <= base->share.height + Params().Recency();
}

bool ShareChainstateManager::AcceptBaseChainShare(const Share& share, const MmrAccumulator& peaks, const std::vector<uint256>& fruits, std::string& reason)
{
    ShareChainstate& state = ActiveChainstate();
    if (!ActiveChain().chain.empty()) {
        reason = "base-chain-not-empty";
        return false;
    }
    if (share.anchor.IsNull()) {
        reason = "base-is-genesis"; // a genesis share validates the normal way
        return false;
    }
    if (share.wit.share_chain_path != Params().ChainId()) {
        reason = "share-bad-chain-id";
        return false;
    }
    // Its bits are a claim too, but never easier than the chain allows.
    if (ShareTargetFor(share.bits) == 0 || ShareTargetFor(share.bits) > ShareTarget()) {
        reason = "share-bad-bits";
        return false;
    }
    bool mutated = false;
    const uint256 pow_hash = share.GetPoWHash(m_options.chainparams.GetConsensus(), &mutated);
    if (mutated || UintToArith256(pow_hash) > ChainTargetFor(share.bits)) {
        reason = "chainshare-high-hash";
        return false;
    }
    if (share.IsAux() && !CheckAuxBinding(share, reason)) {
        return false;
    }
    if (peaks.GetHash() != share.prev_root) {
        reason = "chainshare-bad-peaks";
        return false;
    }
    if (!std::is_sorted(fruits.begin(), fruits.end()) || std::adjacent_find(fruits.begin(), fruits.end()) != fruits.end()) {
        reason = "chainshare-fruits-unsorted";
        return false;
    }
    if (FruitsRoot(fruits) != share.fruits_root) {
        reason = "chainshare-bad-fruits-root";
        return false;
    }
    {
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(m_options.adjusted_time_callback().time_since_epoch()).count();
        if (static_cast<int64_t>(share.wit.nTime) > now + 2 * 60 * 60) {
            reason = "share-time-too-new";
            return false;
        }
    }
    const uint256 hash = share.GetHash();
    state.fruits.emplace(hash, share);
    auto index = std::make_shared<ShareIndex>();
    index->share = share;
    index->peaks = peaks;
    index->fruits = fruits;
    index->base_total_work = share.total_work;
    state.chain_shares.emplace(hash, index);
    SetActiveTip(index);
    LogPrintf("Share chain: starting from trusted base %s at height %d\n", hash.ToString(), share.height);
    return true;
}

void ShareChainstateManager::SetActiveTip(const std::shared_ptr<ShareIndex>& tip)
{
    std::vector<std::shared_ptr<ShareIndex>> chain;
    for (std::shared_ptr<const ShareIndex> cur = tip; cur; cur = cur->prev_share) {
        chain.push_back(std::const_pointer_cast<ShareIndex>(cur));
    }
    std::reverse(chain.begin(), chain.end());
    ActiveChain().base = chain.empty() ? 0 : static_cast<int>(chain.front()->share.height);
    ActiveChain().chain = std::move(chain);
}

void ShareChainstateManager::Prune()
{
    ShareChainstate& state = ActiveChainstate();
    const std::shared_ptr<ShareIndex> tip = ActiveTip();
    if (!tip) return;
    // Window floor: walk back the work the PPLNS window needs (sized like CheckPayouts does).
    const arith_uint256 window = GetShareWork(tip->share.wit.nBits) * Params().WindowK();
    arith_uint256 acc = 0;
    int floor_height = static_cast<int>(tip->share.height);
    for (const ShareIndex* cur = tip.get(); cur; cur = cur->prev_share.get()) {
        floor_height = static_cast<int>(cur->share.height);
        for (const uint256& f : cur->fruits) {
            auto it = state.fruits.find(f);
            if (it != state.fruits.end()) acc += GetShareWork(it->second.bits);
        }
        if (acc >= window) break;
    }
    const int keep_from = std::max(0, floor_height - static_cast<int>(Params().Recency()) - 64);
    if (keep_from <= state.chain.base) return;
    // Drop chain shares below keep_from (active chain and forks) and cut the links into them.
    for (auto it = state.chain_shares.begin(); it != state.chain_shares.end();) {
        if (static_cast<int>(it->second->share.height) < keep_from) it = state.chain_shares.erase(it);
        else ++it;
    }
    for (auto& item : state.chain_shares) {
        if (item.second->prev_share && static_cast<int>(item.second->prev_share->share.height) < keep_from) item.second->prev_share = nullptr;
    }
    // Drop fruits whose anchor is gone (they can never be listed or verified again).
    size_t dropped = 0;
    for (auto it = state.fruits.begin(); it != state.fruits.end();) {
        const bool is_chain = state.chain_shares.count(it->first) > 0;
        const bool anchor_gone = it->second.anchor.IsNull() ? keep_from > 0 : !state.chain_shares.count(it->second.anchor);
        if (!is_chain && anchor_gone) { it = state.fruits.erase(it); ++dropped; } else ++it;
    }
    SetActiveTip(tip);
    LogPrint(BCLog::NET, "Share chain: pruned below height %d (%d chain shares, %d fruits kept, %d fruits dropped)\n", keep_from, state.chain_shares.size(), state.fruits.size(), dropped);
}

Share ShareFromTemplateState(const ShareChainstateManager::TemplateState& st, const WitnessUnknown& miner)
{
    Share share;
    share.version = 0; // native witness
    share.bits = st.bits;
    share.height = st.height;
    share.total_work = st.total_work;
    share.anchor = st.anchor;
    share.prev_root = st.prev_root;
    share.fruits_root = st.fruits_root;
    share.miner = miner;
    return share;
}

bool FillNativeShareWitness(Share& share, const CMutableTransaction& cb, const std::vector<uint256>& cb_branch, const CBlockHeader& header, const uint256& chain_id)
{
    DataStream ds{};
    ds << TX_NO_WITNESS(cb);
    const Span<const unsigned char> span = MakeUCharSpan(ds);
    const std::vector<unsigned char> bytes(span.begin(), span.end());
    // tail = root(32) || id(4) || nLockTime(4) || lock_height(4)
    if (bytes.size() < 44 || cb.vout.empty()) {
        return false;
    }
    share.wit.commit.clear(); // single-entry Merkle map: the branch is empty
    share.wit.cb1.assign(bytes.begin(), bytes.end() - 44);
    share.wit.nLockTime = cb.nLockTime;
    share.wit.lock_height = cb.lock_height;
    share.wit.branch = cb_branch;
    share.wit.nVersion = header.nVersion;
    share.wit.hashPrevBlock = header.hashPrevBlock;
    share.wit.share_chain_path = chain_id;
    share.wit.nTime = header.nTime;
    share.wit.nBits = header.nBits;
    share.wit.nNonce = header.nNonce;
    return true;
}

namespace {
struct ToWitnessUnknown {
    std::optional<WitnessUnknown> operator()(const CNoDestination&) const { return std::nullopt; }
    std::optional<WitnessUnknown> operator()(const PubKeyDestination&) const { return std::nullopt; }
    std::optional<WitnessUnknown> operator()(const PKHash&) const { return std::nullopt; }
    std::optional<WitnessUnknown> operator()(const ScriptHash&) const { return std::nullopt; }
    std::optional<WitnessUnknown> operator()(const PayToAnchor&) const { return std::nullopt; }
    std::optional<WitnessUnknown> operator()(const WitnessV0LongHash& id) const { return WitnessUnknown(0u, std::vector<unsigned char>(id.begin(), id.end())); }
    std::optional<WitnessUnknown> operator()(const WitnessV0ShortHash& id) const { return WitnessUnknown(0u, std::vector<unsigned char>(id.begin(), id.end())); }
    std::optional<WitnessUnknown> operator()(const WitnessUnknown& id) const { return id; }
};
} // namespace

std::optional<WitnessUnknown> WitnessUnknownFromDestination(const CTxDestination& dest)
{
    return std::visit(ToWitnessUnknown(), dest);
}

ShareChainstateManager* g_sharechainman = nullptr;

uint32_t ShareChainstateManager::RequiredShareBits(const ShareIndex* anchor) const
{
    const ShareChainParams& p = Params();
    if (!p.Retarget() || !anchor || !anchor->prev_share) {
        return p.ShareBits();
    }
    // LWMA over the last N chain shares ending at the anchor (newest weighs most).
    constexpr size_t N = 60;
    std::vector<const ShareIndex*> v;
    for (const ShareIndex* cur = anchor; cur && v.size() < N; cur = cur->prev_share.get()) {
        v.push_back(cur);
    }
    if (v.size() < 2) {
        return p.ShareBits();
    }
    // The average runs over chain shares, which come one per ChainFactor shares.
    const int64_t T = p.ShareInterval() * static_cast<int64_t>(std::max<uint32_t>(1, p.ChainFactor()));
    int64_t sum_wt = 0, sum_w = 0;
    arith_uint256 sum_target = 0;
    size_t k = 0;
    for (size_t i = v.size() - 1; i > 0; --i) { // oldest pair first
        const int64_t prev_t = v[i]->share.wit.nTime;
        const int64_t cur_t = v[i - 1]->share.wit.nTime;
        const int64_t solve = std::clamp<int64_t>(cur_t - prev_t, 1, 6 * T);
        ++k;
        sum_wt += static_cast<int64_t>(k) * solve;
        sum_w += static_cast<int64_t>(k);
        arith_uint256 t;
        t.SetCompact(v[i - 1]->share.bits);
        sum_target += t;
    }
    const arith_uint256 avg_target = sum_target / static_cast<uint64_t>(k);
    const int64_t lwma = std::max<int64_t>(1, sum_wt / sum_w);
    arith_uint256 next = avg_target * static_cast<uint64_t>(lwma) / static_cast<uint64_t>(T);
    // Per-step clamp against the anchor's target, and never easier than the chain's easiest target.
    arith_uint256 anchor_target;
    anchor_target.SetCompact(anchor->share.bits);
    const arith_uint256 upper = anchor_target / 4 * 5;
    const arith_uint256 lower = anchor_target / 4 * 3;
    if (next > upper) next = upper;
    if (next < lower) next = lower;
    arith_uint256 easiest;
    easiest.SetCompact(p.ShareBits());
    if (next > easiest) next = easiest;
    return next.GetCompact();
}

std::optional<ShareMessage> ShareChainstateManager::GetShareMessage(const uint256& hash) const
{
    const ShareChainstate& state = ActiveChainstate();
    ShareMessage msg;
    auto cs = state.chain_shares.find(hash);
    if (cs != state.chain_shares.end()) {
        msg.share = cs->second->share;
        msg.has_body = true;
        msg.body.peaks = cs->second->peaks;
        msg.body.fruits = cs->second->fruits;
        return msg;
    }
    auto f = state.fruits.find(hash);
    if (f != state.fruits.end()) {
        msg.share = f->second;
        return msg;
    }
    return std::nullopt;
}

ShareAccept ShareChainstateManager::ProcessShareMessage(const ShareMessage& msg, std::vector<uint256>& missing, std::string& reason, bool live, bool allow_base)
{
    ShareChainstate& state = ActiveChainstate();
    const uint256 hash = msg.share.GetHash();
    missing.clear();
    const bool anchor_unknown = !msg.share.anchor.IsNull() && !state.chain_shares.count(msg.share.anchor);
    if (anchor_unknown) {
        // A node with no chain yet takes the first chain share of a sync batch as its base.
        if (allow_base && msg.has_body && ActiveChain().chain.empty()) {
            if (state.chain_shares.count(hash)) {
                return ShareAccept::DUPLICATE;
            }
            return AcceptBaseChainShare(msg.share, msg.body.peaks, msg.body.fruits, reason) ? ShareAccept::ACCEPTED : ShareAccept::INVALID;
        }
        // Below a trusted base nothing can be verified and nothing is needed.
        const std::shared_ptr<ShareIndex> base = ActiveChain().Genesis();
        if (base && !base->share.anchor.IsNull() && msg.share.height <= base->share.height) {
            reason = "share-below-base";
            return ShareAccept::INVALID;
        }
        missing.push_back(msg.share.anchor);
    }
    if (msg.has_body) {
        const ShareIndex* parent = (!anchor_unknown && !msg.share.anchor.IsNull()) ? state.chain_shares.at(msg.share.anchor).get() : nullptr;
        const bool near_base = NearBase(parent, msg.share.height);
        for (const uint256& f : msg.body.fruits) {
            if (!state.fruits.count(f) && !near_base) missing.push_back(f);
        }
    }
    if (!missing.empty()) {
        reason = "share-missing-dependencies";
        return ShareAccept::MISSING;
    }
    if (msg.has_body) {
        if (state.chain_shares.count(hash)) {
            return ShareAccept::DUPLICATE;
        }
        if (AcceptChainShare(msg.share, msg.body.peaks, msg.body.fruits, reason, live)) return ShareAccept::ACCEPTED;
        // the Freicoin block it builds on has not reached us yet: wait for it
        return reason == "share-unknown-prev-block" ? ShareAccept::MISSING : ShareAccept::INVALID;
    }
    if (state.fruits.count(hash)) {
        return ShareAccept::DUPLICATE;
    }
    if (AcceptFruit(msg.share, reason, live)) return ShareAccept::ACCEPTED;
    return reason == "share-unknown-prev-block" ? ShareAccept::MISSING : ShareAccept::INVALID;
}

void ShareChainstateManager::AddOrphan(const ShareMessage& msg)
{
    if (m_orphans.size() >= 5000) {
        m_orphans.erase(m_orphans.begin());
    }
    m_orphans[msg.share.GetHash()] = msg;
}

std::vector<ShareMessage> ShareChainstateManager::TakeOrphans()
{
    std::vector<ShareMessage> out;
    out.reserve(m_orphans.size());
    for (auto& item : m_orphans) {
        out.push_back(std::move(item.second));
    }
    m_orphans.clear();
    return out;
}

namespace {
constexpr uint64_t SHARECHAIN_DUMP_VERSION = 2;
} // namespace

bool ShareChainstateManager::DumpToFile(const fs::path& path) const
{
    const ShareChainstate& state = ActiveChainstate();
    // Chain shares by height so parents load first, then the remaining fruits.
    std::vector<std::shared_ptr<ShareIndex>> chain_shares;
    for (const auto& item : state.chain_shares) chain_shares.push_back(item.second);
    std::sort(chain_shares.begin(), chain_shares.end(), [](const auto& a, const auto& b) { return a->share.height < b->share.height; });
    std::vector<ShareMessage> entries;
    std::set<uint256> seen;
    for (const auto& cs : chain_shares) {
        // The fruits a chain share lists hang from its ancestors, so they can be
        // accepted as soon as they precede it in the file.
        for (const uint256& f : cs->fruits) {
            if (seen.count(f) || state.chain_shares.count(f)) continue;
            auto it = state.fruits.find(f);
            if (it == state.fruits.end()) continue;
            ShareMessage m;
            m.share = it->second;
            entries.push_back(std::move(m));
            seen.insert(f);
        }
        ShareMessage m;
        m.share = cs->share;
        m.has_body = true;
        m.body.peaks = cs->peaks;
        m.body.fruits = cs->fruits;
        entries.push_back(std::move(m));
        seen.insert(cs->GetHash());
    }
    for (const auto& item : state.fruits) {
        if (seen.count(item.first)) continue;
        ShareMessage m;
        m.share = item.second;
        entries.push_back(std::move(m));
        seen.insert(item.first);
    }
    // Shares still waiting for a dependency (a block, an anchor): keep them too.
    for (const auto& item : m_orphans) {
        if (seen.count(item.first)) continue;
        entries.push_back(item.second);
    }
    const fs::path tmp = path + ".new";
    AutoFile file{fsbridge::fopen(tmp, "wb")};
    if (file.IsNull()) {
        LogPrintf("Share chain: cannot open %s for writing\n", fs::PathToString(tmp));
        return false;
    }
    try {
        file << SHARECHAIN_DUMP_VERSION;
        // the active tip, so equal-work forks resolve the same way after a restart
        const std::shared_ptr<ShareIndex> tip = ActiveTip();
        file << (tip ? tip->GetHash() : uint256());
        file << static_cast<uint64_t>(entries.size());
        for (const ShareMessage& m : entries) {
            file << m;
        }
        if (!file.Commit()) {
            throw std::runtime_error("Commit failed");
        }
        file.fclose();
        if (!RenameOver(tmp, path)) {
            throw std::runtime_error("Rename failed");
        }
    } catch (const std::exception& e) {
        LogPrintf("Share chain: failed to dump %s: %s\n", fs::PathToString(path), e.what());
        return false;
    }
    LogPrintf("Share chain: dumped %d chain shares and %d other shares to %s\n", chain_shares.size(), entries.size() - chain_shares.size(), fs::PathToString(path));
    return true;
}

bool ShareChainstateManager::LoadFromFile(const fs::path& path)
{
    AutoFile file{fsbridge::fopen(path, "rb")};
    if (file.IsNull()) {
        LogPrintf("Share chain: no %s to load\n", fs::PathToString(path));
        return false;
    }
    size_t accepted = 0, rejected = 0, deferred = 0;
    uint256 saved_tip;
    try {
        uint64_t version = 0, count = 0;
        file >> version;
        if (version != SHARECHAIN_DUMP_VERSION) {
            LogPrintf("Share chain: unsupported dump version %d\n", version);
            return false;
        }
        file >> saved_tip;
        file >> count;
        std::vector<ShareMessage> deferred_msgs;
        std::set<uint256> in_file;
        for (uint64_t i = 0; i < count; ++i) {
            ShareMessage m;
            file >> m;
            if (m.has_body) in_file.insert(m.share.GetHash());
            std::vector<uint256> missing;
            std::string reason;
            const ShareAccept r = ProcessShareMessage(m, missing, reason, /*live=*/false);
            if (r == ShareAccept::ACCEPTED || r == ShareAccept::DUPLICATE) ++accepted;
            else if (r == ShareAccept::MISSING) { deferred_msgs.push_back(m); ++deferred; }
            else {
                ++rejected;
                if (rejected <= 5) LogPrintf("Share chain: entry %d (%s, height %d) rejected at load: %s\n", i, m.has_body ? "chain share" : "fruit", m.share.height, reason);
            }
        }
        // Entries whose dependencies came later in the file: retry until a pass
        // makes no progress (dependencies can chain several levels deep).
        while (!deferred_msgs.empty()) {
            std::vector<ShareMessage> still;
            for (const ShareMessage& m : deferred_msgs) {
                std::vector<uint256> missing;
                std::string reason;
                const ShareAccept r = ProcessShareMessage(m, missing, reason, /*live=*/false);
                if (r == ShareAccept::ACCEPTED || r == ShareAccept::DUPLICATE) { ++accepted; --deferred; }
                else if (r == ShareAccept::MISSING) still.push_back(m);
                else { ++rejected; --deferred; }
            }
            if (still.size() == deferred_msgs.size()) {
                // Nothing resolves: a pruned dump starts above its anchors. Our own
                // file is trusted, so its lowest chain share becomes the base.
                if (!ActiveChain().chain.empty()) break;
                auto lowest = still.end();
                for (auto it = still.begin(); it != still.end(); ++it) {
                    if (!it->has_body || in_file.count(it->share.anchor)) continue; // waits for something in the file
                    if (lowest == still.end() || it->share.height < lowest->share.height) lowest = it;
                }
                if (lowest == still.end()) {
                    size_t shown = 0;
                    for (const ShareMessage& m : still) {
                        if (shown++ >= 5) break;
                        std::vector<uint256> missing;
                        std::string reason;
                        ProcessShareMessage(m, missing, reason, /*live=*/false);
                        LogPrintf("Share chain: %s %s (height %d, anchor %s) unresolved at load: %s, %d missing%s\n", m.has_body ? "chain share" : "fruit", m.share.GetHash().ToString(), m.share.height, m.share.anchor.ToString().substr(0, 16), reason, missing.size(), missing.empty() ? "" : (" first " + missing.front().ToString().substr(0, 16)));
                    }
                    break;
                }
                std::vector<uint256> missing;
                std::string reason;
                if (ProcessShareMessage(*lowest, missing, reason, /*live=*/false, /*allow_base=*/true) != ShareAccept::ACCEPTED) {
                    LogPrintf("Share chain: cannot start from %s: %s\n", lowest->share.GetHash().ToString(), reason);
                    break;
                }
                ++accepted;
                --deferred;
                still.erase(lowest);
            }
            deferred_msgs = std::move(still);
        }
        // Whatever still waits (typically shares built on blocks this node has not
        // connected yet, after a restart that lost unflushed chain state) goes to the
        // orphan pool: it is retried as blocks connect and dumped again at shutdown.
        for (const ShareMessage& m : deferred_msgs) {
            AddOrphan(m);
        }
    } catch (const std::exception& e) {
        LogPrintf("Share chain: failed to load %s: %s\n", fs::PathToString(path), e.what());
        return false;
    }
    // Restore the tip we had: among equal-work forks the load order is arbitrary.
    {
        auto it = ActiveChainstate().chain_shares.find(saved_tip);
        if (it != ActiveChainstate().chain_shares.end()) {
            SetActiveTip(it->second);
        }
    }
    Prune();
    LogPrintf("Share chain: loaded %d shares from %s (%d rejected, %d unresolved), tip height %d, base %d\n", accepted, fs::PathToString(path), rejected, deferred, ActiveHeight(), BaseHeight());
    return true;
}

std::vector<uint256> ShareChainstateManager::GetShareLocator() const
{
    std::vector<uint256> locator;
    const ShareChain& chain = ActiveChain();
    int h = chain.Height();
    int step = 1;
    while (h >= chain.base && locator.size() < 32) {
        locator.push_back(chain[h]->GetHash());
        if (locator.size() >= 10) step *= 2;
        h -= step;
    }
    return locator;
}

std::vector<ShareMessage> ShareChainstateManager::GetShareRange(const std::vector<uint256>& locator, size_t max_entries) const
{
    const ShareChainstate& state = ActiveChainstate();
    const ShareChain& chain = ActiveChain();
    int start = 0;
    for (const uint256& h : locator) {
        auto it = state.chain_shares.find(h);
        if (it == state.chain_shares.end()) continue;
        const int height = static_cast<int>(it->second->share.height);
        if (height <= chain.Height() && chain[height] && chain[height]->GetHash() == h) {
            start = height + 1;
            break;
        }
    }
    start = std::max(start, chain.base);
    std::vector<ShareMessage> out;
    std::set<uint256> sent;
    // Emit a chain share after everything it depends on: its listed fruits, and for a listed
    // fruit that is itself a (forked) chain share, that one's dependencies too.
    std::function<void(const ShareIndex&)> emit_chain_share = [&](const ShareIndex& cs) {
        if (sent.count(cs.GetHash())) return;
        for (const uint256& f : cs.fruits) {
            if (sent.count(f)) continue;
            auto fork = state.chain_shares.find(f);
            if (fork != state.chain_shares.end()) {
                emit_chain_share(*fork->second);
                continue;
            }
            auto fit = state.fruits.find(f);
            if (fit == state.fruits.end()) continue;
            ShareMessage m;
            m.share = fit->second;
            out.push_back(std::move(m));
            sent.insert(f);
        }
        ShareMessage m;
        m.share = cs.share;
        m.has_body = true;
        m.body.peaks = cs.peaks;
        m.body.fruits = cs.fruits;
        out.push_back(std::move(m));
        sent.insert(cs.GetHash());
    };
    for (int h = start; h <= chain.Height() && out.size() < max_entries; ++h) {
        emit_chain_share(*chain[h]);
    }
    // Fruits nobody has listed yet (still listable from the tip) so the peer ends up with the
    // same state and can list them itself.
    if (out.size() < max_entries) {
        for (const uint256& f : ListableFruits(chain.Tip().get())) {
            if (sent.count(f)) continue;
            auto fork = state.chain_shares.find(f);
            if (fork != state.chain_shares.end()) {
                emit_chain_share(*fork->second);
                continue;
            }
            auto fit = state.fruits.find(f);
            if (fit == state.fruits.end()) continue;
            ShareMessage m;
            m.share = fit->second;
            out.push_back(std::move(m));
            sent.insert(f);
            if (out.size() >= max_entries) break;
        }
    }
    return out;
}

static RPCHelpMan getsharechaininfo() {
    return RPCHelpMan{"getsharechaininfo",
        "\nReturns an object containing various state info regarding the share chain.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::BOOL, "enabled", "whether the share chain service is enabled"},
            {RPCResult::Type::STR, "network", /*optional=*/true, "which share chain the mining service is using"},
            {RPCResult::Type::STR_HEX, "sharebits", /*optional=*/true, "the chain's initial (and easiest) compact share target"},
            {RPCResult::Type::STR_HEX, "nextbits", /*optional=*/true, "compact share target the next share must meet (retargeted)"},
            {RPCResult::Type::NUM, "nextdifficulty", /*optional=*/true, "the same as a difficulty"},
            {RPCResult::Type::NUM, "chainfactor", /*optional=*/true, "a chain share must meet share target / chainfactor"},
            {RPCResult::Type::NUM, "recency", /*optional=*/true, "chain heights a fruit stays listable after its anchor"},
            {RPCResult::Type::NUM, "height", /*optional=*/true, "height of the active share chain tip (-1 when empty)"},
            {RPCResult::Type::STR_HEX, "tip", /*optional=*/true, "hash of the active share chain tip (empty when none)"},
            {RPCResult::Type::NUM, "chainshares", /*optional=*/true, "known chain shares (all forks)"},
            {RPCResult::Type::NUM, "fruits", /*optional=*/true, "known shares"},
            {RPCResult::Type::NUM, "baseheight", /*optional=*/true, "lowest chain height still kept (older shares were pruned)"},
        }},
        RPCExamples{
            HelpExampleCli("getsharechaininfo", "")
            + HelpExampleRpc("getsharechaininfo", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("enabled", g_share_chain_params != nullptr);
    if (!g_share_chain_params) {
        return obj;
    }
    const auto& shareparams = *g_share_chain_params;
    obj.pushKV("network", shareparams.GetShareChainTypeString());
    obj.pushKV("sharebits", strprintf("%08x", shareparams.ShareBits()));
    obj.pushKV("chainfactor", (int)shareparams.ChainFactor());
    obj.pushKV("recency", (int)shareparams.Recency());
    const node::NodeContext& node = EnsureAnyNodeContext(request.context);
    if (node.sharechainman) {
        LOCK(::cs_main);
        const ShareChainstateManager& man = *node.sharechainman;
        const std::shared_ptr<ShareIndex> tip = man.ActiveTip();
        obj.pushKV("height", tip ? (int64_t)tip->share.height : -1);
        obj.pushKV("tip", tip ? tip->GetHash().GetHex() : "");
        obj.pushKV("chainshares", (int64_t)man.ActiveChainstate().chain_shares.size());
        obj.pushKV("fruits", (int64_t)man.ActiveChainstate().fruits.size());
        obj.pushKV("baseheight", (int64_t)man.BaseHeight());
        const uint32_t next_bits = man.RequiredShareBits(tip.get());
        obj.pushKV("nextbits", strprintf("%08x", next_bits));
        arith_uint256 next_target;
        next_target.SetCompact(next_bits);
        arith_uint256 one_target;
        one_target.SetCompact(0x1d00ffff); // difficulty 1
        obj.pushKV("nextdifficulty", next_target == 0 ? 0.0 : one_target.getdouble() / next_target.getdouble());
    }
    return obj;
},
    };
}

void RegisterShareChainRPCCommands(CRPCTable& t) {
    static const CRPCCommand commands[]{
        {"mining", &getsharechaininfo},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}

// End of File

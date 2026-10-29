// Copyright (c) 2020-2023 The Freicoin Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sharechain.h>

#include <addresstype.h> // for ExtractDestination, GetScriptForDestination
#include <chainparams.h> // for Params()

#include <consensus/merkle.h> // for ComputeMerkleMapRootFromBranch, ComputeMerkleRootFromBranch
#include <hash.h> // for CHash256, HashWriter
#include <key_io.h> // for EncodeDestination, DecodeSecret, DecodeDestination
#include <key.h> // for CKey
#include <pubkey.h> // for CPubKey
#include <crypto/sha256.h> // for CSHA256
#include <net_processing.h> // for PeerManager::RelayShareTx
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
#include <rpc/util.h> // for ParseHashV

namespace {
//! Transactions a chain share may carry, and the pool behind the templates.
constexpr size_t MAX_TXS_PER_CHAIN_SHARE = 100;
constexpr size_t MAX_TXPOOL = 1000;
} // namespace

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
        m_chain_id = uint256S("5348415245434841494e2d4d41494e0000000000000000000000000000000006"); // 'SHARECHAIN-MAIN' || 6 (2: aux identity = parent hash; 3: committed payouts; 4: window capped at WindowShares; 5: transferable credits; 6: credit ledger with amounts)
        m_window_shares = 4320;      // 3 days of chain shares at 60 s
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
        m_chain_id = uint256S("5348415245434841494e2d5245475445535400000000000000000000000006");
        m_window_shares = 48;        // small enough for a unit test to cross it
        // A regtest block is two hashes, so WINDOW_K blocks of work would be a
        // window of four fruits: credits slid out before the next chain share
        // (seen in the functional test).  The work rule is moot here; the
        // window is WINDOW_SHARES chain shares.
        m_window_k = 1u << 20;
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
    swap(lhs.cb_hash, rhs.cb_hash);
    swap(lhs.cb_version, rhs.cb_version);
    swap(lhs.script_sig, rhs.script_sig);
    swap(lhs.sequence, rhs.sequence);
    swap(lhs.total, rhs.total);
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
    ss << txs_root;
    ss << COMPACTSIZE(miner.GetWitnessVersion()); // will always be single byte
    ss << miner.GetWitnessProgram();
    return ss.GetHash();
}

CBlockHeader Share::GetBlockHeader(bool *mutated) const {
    if (mutated) {
        *mutated = false;
    }

    // The commitment branch must at least be well formed (the root it yields is
    // checked when the coinbase is rebuilt against cb_hash).
    bool invalid = false;
    (void)ComputeMerkleMapRootFromBranch(GetHeaderHash(), wit.commit, wit.share_chain_path, &invalid);
    if (invalid && mutated) {
        *mutated = true;
    }

    // hashMerkleRoot from the coinbase txid the witness carries.
    uint256 hash = ComputeMerkleRootFromBranch(wit.cb_hash, wit.branch, 0);

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

std::string ShareAddressString(const WitnessUnknown& miner)
{
    CTxDestination dest;
    if (!ExtractDestination(GetScriptForDestination(miner), dest)) return {};
    return EncodeDestination(dest);
}

uint256 ShareTx::GetSigHash(const uint256& chain_id) const
{
    HashWriter ss{};
    ss << std::string("SHARECHAIN-TRANSFER") << chain_id << type;
    ss << COMPACTSIZE(from.GetWitnessVersion()) << from.GetWitnessProgram();
    ss << COMPACTSIZE(to.GetWitnessVersion()) << to.GetWitnessProgram();
    ss << amount << min_height << max_height << expiry << hashlock << pubkey;
    return ss.GetHash();
}

uint256 ShareTx::GetHash() const
{
    HashWriter ss{};
    ss << *this;
    return ss.GetHash();
}

uint256 TxsRoot(const std::vector<ShareTx>& txs)
{
    if (txs.empty()) return uint256();
    std::vector<uint256> ids;
    ids.reserve(txs.size());
    for (const ShareTx& tx : txs) ids.push_back(tx.GetHash());
    return ComputeMerkleRoot(ids, nullptr);
}

uint256 ShareHashLock(const uint256& preimage)
{
    uint256 out;
    CSHA256().Write(preimage.begin(), 32).Finalize(out.begin());
    return out;
}

bool CheckShareTxForm(const ShareTx& tx, const uint256& chain_id, std::string& reason)
{
    if (tx.type == ShareTx::CLAIM) {
        if (tx.transfer_id.IsNull()) { reason = "tx-bad-claim"; return false; }
        return true;
    }
    if (tx.type != ShareTx::TRANSFER) { reason = "tx-bad-type"; return false; }
    // from: a version-0 key hash (the only thing a key can sign for); to: any witness program
    if (tx.from.GetWitnessVersion() != 0 || tx.from.GetWitnessProgram().size() != 20) { reason = "tx-bad-from"; return false; }
    const std::vector<unsigned char>& to_program = tx.to.GetWitnessProgram();
    if (tx.to.GetWitnessVersion() > 16 || to_program.size() < 2 || to_program.size() > 40) { reason = "tx-bad-to"; return false; }
    if (tx.from == tx.to) { reason = "tx-self-transfer"; return false; }
    if (tx.hashlock.IsNull() != (tx.expiry == 0)) { reason = "tx-bad-lock"; return false; }
    const CPubKey pubkey(tx.pubkey);
    if (tx.pubkey.size() != CPubKey::COMPRESSED_SIZE || !pubkey.IsFullyValid()) { reason = "tx-bad-pubkey"; return false; }
    // Freicoin's key address: the short hash of the P2WPK script, not hash160(pubkey).
    const WitnessV0ShortHash id(0, pubkey);
    if (!std::equal(id.begin(), id.end(), tx.from.GetWitnessProgram().begin())) { reason = "tx-key-mismatch"; return false; }
    if (!pubkey.Verify(tx.GetSigHash(chain_id), tx.signature)) { reason = "tx-bad-signature"; return false; }
    return true;
}

void ShareLedger::Mint(const WitnessUnknown& miner, uint32_t height, const arith_uint256& work)
{
    Bucket& b = balance[miner][height];
    b.work += work;
    b.fruits += 1;
}

void ShareLedger::Merge(Buckets& into, const Buckets& from)
{
    for (const auto& item : from) {
        Bucket& b = into[item.first];
        b.work += item.second.work;
        b.fruits += item.second.fruits;
    }
}

void ShareLedger::Expire(uint32_t height)
{
    for (auto it = locks.begin(); it != locks.end();) {
        if (it->second.tx.expiry >= height) { ++it; continue; }
        Merge(balance[it->second.tx.from], it->second.held);
        it = locks.erase(it);
    }
}

bool ShareLedger::Apply(const ShareTx& tx, uint32_t height, uint32_t max_lock, bool strict, std::string& reason)
{
    const uint256 txid = tx.GetHash();
    if (seen.count(txid)) {
        if (strict) { reason = "tx-replay"; return false; }
        return true;
    }
    if (tx.type == ShareTx::TRANSFER) {
        if (strict && tx.max_height > height) { reason = "tx-future-height"; return false; }
        if (strict && tx.min_height > tx.max_height) { reason = "tx-bad-range"; return false; }
        if (strict && tx.IsLocked() && (tx.expiry <= height || tx.expiry > height + max_lock)) { reason = "tx-bad-expiry"; return false; }
        const arith_uint256 wanted = UintToArith256(tx.amount);
        const bool all = wanted == 0;
        auto src = balance.find(tx.from);
        arith_uint256 available = 0;
        if (src != balance.end()) {
            for (const auto& b : src->second) {
                if (b.first >= tx.min_height && b.first <= tx.max_height) available += b.second.work;
            }
        }
        if (available == 0) {
            if (strict) { reason = "tx-moves-nothing"; return false; }
            seen.insert(txid);
            return true;
        }
        if (strict && !all && available < wanted) { reason = "tx-insufficient-credit"; return false; }
        seen.insert(txid);
        // Newest buckets first, up to the amount (or everything in the range).
        arith_uint256 remaining = wanted;
        Buckets moved;
        for (auto it = src->second.rbegin(); it != src->second.rend() && (all || remaining > 0); ++it) {
            if (it->first < tx.min_height || it->first > tx.max_height) continue;
            Bucket& b = it->second;
            const arith_uint256 take = (all || remaining >= b.work) ? b.work : remaining;
            if (take == 0) continue;
            const int64_t fruits_take = take == b.work ? b.fruits : static_cast<int64_t>((arith_uint256(static_cast<uint64_t>(b.fruits)) * take / b.work).GetLow64());
            Bucket& m = moved[it->first];
            m.work += take;
            m.fruits += fruits_take;
            b.work -= take;
            b.fruits -= fruits_take;
            if (!all) remaining -= take;
        }
        for (auto it = src->second.begin(); it != src->second.end();) {
            if (it->second.work == 0) it = src->second.erase(it); else ++it;
        }
        if (src->second.empty()) balance.erase(src);
        if (tx.IsLocked()) locks[txid] = Lock{tx, height, std::move(moved)};
        else Merge(balance[tx.to], moved);
        return true;
    }
    // claim
    auto it = locks.find(tx.transfer_id);
    if (it == locks.end()) {
        if (strict) { reason = "tx-unknown-transfer"; return false; }
        seen.insert(txid);
        return true;
    }
    if (ShareHashLock(tx.preimage) != it->second.tx.hashlock) {
        if (strict) { reason = "tx-bad-preimage"; return false; }
        seen.insert(txid);
        return true;
    }
    seen.insert(txid);
    Merge(balance[it->second.tx.to], it->second.held);
    locks.erase(it);
    return true;
}

void ShareLedger::Credits(std::map<WitnessUnknown, arith_uint256>& credits, std::map<WitnessUnknown, int64_t>& fruits) const
{
    for (const auto& item : balance) {
        for (const auto& b : item.second) {
            credits[item.first] += b.second.work;
            fruits[item.first] += b.second.fruits;
        }
    }
    for (const auto& item : locks) {
        for (const auto& b : item.second.held) {
            credits[item.second.tx.from] += b.second.work;
            fruits[item.second.tx.from] += b.second.fruits;
        }
    }
}

WindowCredits ComputeWindowCredits(const ShareIndex* anchor, const arith_uint256& window_work, size_t window_shares, const FruitLookup& lookup)
{
    WindowCredits out;
    // Back to the floor: the chain shares whose listed fruits fill the window.
    std::vector<const ShareIndex*> chain;
    arith_uint256 acc = 0;
    for (const ShareIndex* cur = anchor; cur && acc < window_work && chain.size() < window_shares; cur = cur->prev_share.get()) {
        chain.push_back(cur);
        for (const uint256& fruit : cur->fruits) {
            const std::optional<FruitInfo> info = lookup(fruit);
            if (info) acc += GetShareWork(info->bits);
        }
    }
    std::reverse(chain.begin(), chain.end());
    // Forward: mint the listed fruits, apply the transactions.  Every transaction
    // that touches credit of this window lies in this window (it comes at or
    // after the listing that minted the credit), so the ledger is complete.
    std::string reason;
    for (const ShareIndex* cs : chain) {
        const uint32_t h = cs->share.height;
        for (const uint256& fruit : cs->fruits) {
            const std::optional<FruitInfo> info = lookup(fruit);
            if (info) out.ledger.Mint(info->miner, h, GetShareWork(info->bits));
        }
        out.ledger.Expire(h);
        for (const ShareTx& tx : cs->txs) out.ledger.Apply(tx, h, 0, /*strict=*/false, reason);
    }
    out.ledger.Credits(out.credits, out.fruits_by_owner);
    for (const auto& item : out.credits) out.work += item.second;
    for (const auto& item : out.fruits_by_owner) out.fruits += item.second;
    out.chain_shares = chain.size();
    out.low_height = chain.empty() ? -1 : static_cast<int>(chain.front()->share.height);
    return out;
}

ShareCredits ComputeShareCredits(const ShareIndex* anchor, const arith_uint256& window_work, size_t window_shares, const FruitLookup& lookup)
{
    return ComputeWindowCredits(anchor, window_work, window_shares, lookup).credits;
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

bool BuildShareCoinbase(const Share& share, const std::vector<std::pair<WitnessUnknown, CAmount>>& payouts, CMutableTransaction& cb)
{
    bool invalid = false;
    const uint256 root = ComputeMerkleMapRootFromBranch(share.GetHeaderHash(), share.wit.commit, share.wit.share_chain_path, &invalid);
    if (invalid) {
        return false;
    }
    cb = CMutableTransaction();
    cb.version = share.wit.cb_version;
    cb.vin.emplace_back(COutPoint(), CScript(share.wit.script_sig.begin(), share.wit.script_sig.end()), share.wit.sequence);
    for (const auto& payout : payouts) {
        cb.vout.emplace_back(payout.second, GetScriptForDestination(payout.first));
    }
    cb.vout.emplace_back(0, ShareCommitmentScript(root));
    cb.nLockTime = share.wit.nLockTime;
    cb.lock_height = share.wit.lock_height;
    return true;
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

uint32_t ShareChainstateManager::WindowBitsFor(const uint256& prev, const CBlockHeader& header, uint32_t fallback) const
{
    if (m_options.window_bits_callback) {
        const std::optional<uint32_t> bits = m_options.window_bits_callback(prev, header);
        if (bits) return *bits;
    }
    return fallback;
}

uint32_t ShareChainstateManager::WindowBits(const Share& share) const
{
    return WindowBitsFor(share.wit.hashPrevBlock, share.GetBlockHeader(nullptr), share.wit.nBits);
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
bool WindowReachesBase(const ShareIndex* anchor, const arith_uint256& window, size_t window_shares, const FruitLookup& lookup)
{
    arith_uint256 acc = 0;
    size_t walked = 0;
    for (const ShareIndex* cur = anchor; cur && acc < window && walked < window_shares; cur = cur->prev_share.get(), ++walked) {
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

bool ShareChainstateManager::CheckPayouts(const Share& share, const ShareIndex* anchor, std::optional<CMutableTransaction>& cb_out, std::string& reason) const
{
    cb_out.reset();
    const CAmount total = share.wit.total;
    if (total <= 0) {
        reason = "share-bad-coinbase";
        return false;
    }
    // The window is sized by the target the block's miners face (aux once merge
    // mining is active), taken from the block chain so that template, check and
    // prune agree.  (Seen live: the template used the aux target and this check the
    // native nBits; with two payees every share failed share-bad-payout.)
    const arith_uint256 window = GetShareWork(WindowBits(share)) * Params().WindowK();
    const FruitLookup lookup = [this](const uint256& h) { return LookupFruit(h); };
    if (WindowReachesBase(anchor, window, Params().WindowShares(), lookup)) {
        // Below our trusted base: the share was built from credits we never saw, so
        // its split cannot be checked.  It is history; its own payout only matters
        // if it became a Freicoin block, which the block chain validated.
        return true;
    }
    ShareCredits credits = ComputeShareCredits(anchor, window, Params().WindowShares(), lookup);
    const auto payouts = ComputeSharePayouts(std::move(credits), total, share.miner, Params().Dust(), Params().MaxPayees());
    // The coinbase is exactly these payouts plus the commitment: rebuild it and
    // compare with the txid the proof of work covers.
    CMutableTransaction cb;
    if (!BuildShareCoinbase(share, payouts, cb)) {
        reason = "share-bad-coinbase";
        return false;
    }
    if (cb.GetHash() != share.wit.cb_hash) {
        LogPrint(BCLog::NET, "Share chain: payout mismatch for share %s: expected %d payouts of %d kria at anchor %s\n", share.GetHash().ToString(), payouts.size(), total, share.anchor.ToString());
        reason = "share-bad-payout";
        return false;
    }
    cb_out = std::move(cb);
    return true;
}

bool ShareChainstateManager::CheckAuxBinding(const Share& share, const CMutableTransaction& cb, std::string& reason) const
{
    // The parent header hash covers the Freicoin template through the aux-pow
    // commit fields; the commit Merkle root is computed from the coinbase with
    // an empty scriptSig (CustomizeCommitHash).  Recompute it from the witness
    // coinbase so the payouts and the share commitment are what was mined.
    if (cb.vin.empty()) {
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
    std::optional<CMutableTransaction> cb;
    if (!CheckPayouts(share, anchor, cb, reason)) {
        return false;
    }
    // The parent header must commit to that very coinbase (not checkable below a
    // trusted base, where the coinbase cannot be rebuilt).
    if (share.IsAux() && cb && !CheckAuxBinding(share, *cb, reason)) {
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
    WindowCredits window = WindowAt(tip.get(), frc_bits);
    st.credits = std::move(window.credits);
    st.payouts = ComputeSharePayouts(st.credits, total, miner, Params().Dust(), Params().MaxPayees());
    // Pool transactions valid on top of the tip, in pool order.
    ShareLedger own = std::move(window.ledger);
    own.Expire(st.height);
    for (const auto& item : m_txpool) {
        std::string reason;
        if (own.Apply(item.second, st.height, Params().MaxLock(), /*strict=*/true, reason)) st.txs.push_back(item.second);
        if (st.txs.size() >= MAX_TXS_PER_CHAIN_SHARE) break;
    }
    st.txs_root = TxsRoot(st.txs);
    return st;
}

WindowCredits ShareChainstateManager::WindowAt(const ShareIndex* anchor, uint32_t frc_bits) const
{
    const arith_uint256 window = GetShareWork(frc_bits) * Params().WindowK();
    const FruitLookup lookup = [this](const uint256& h) { return LookupFruit(h); };
    return ComputeWindowCredits(anchor, window, Params().WindowShares(), lookup);
}

bool ShareChainstateManager::AddShareTx(const ShareTx& tx, std::string& reason)
{
    const uint256 txid = tx.GetHash();
    if (m_txpool.count(txid)) { reason = "sharetx-duplicate"; return false; }
    if (!CheckShareTxForm(tx, Params().ChainId(), reason)) return false;
    const std::shared_ptr<ShareIndex> tip = ActiveTip();
    if (!tip) { reason = "tx-moves-nothing"; return false; }
    const uint32_t height = tip->share.height + 1;
    ShareLedger own = WindowAt(tip.get(), WindowBits(tip->share)).ledger;
    own.Expire(height);
    // Behind whatever the pool already holds, as a template would apply it.
    std::string ignored;
    for (const auto& item : m_txpool) own.Apply(item.second, height, Params().MaxLock(), /*strict=*/false, ignored);
    if (!own.Apply(tx, height, Params().MaxLock(), /*strict=*/true, reason)) return false;
    if (m_txpool.size() >= MAX_TXPOOL) { reason = "sharetx-pool-full"; return false; }
    m_txpool.emplace(txid, tx);
    return true;
}

std::vector<ShareTx> ShareChainstateManager::PoolTxs() const
{
    std::vector<ShareTx> out;
    for (const auto& item : m_txpool) out.push_back(item.second);
    return out;
}

void ShareChainstateManager::PruneTxPool()
{
    const std::shared_ptr<ShareIndex> tip = ActiveTip();
    if (!tip) { m_txpool.clear(); return; }
    const uint32_t height = tip->share.height + 1;
    ShareLedger own = WindowAt(tip.get(), WindowBits(tip->share)).ledger;
    own.Expire(height);
    for (auto it = m_txpool.begin(); it != m_txpool.end();) {
        std::string reason;
        if (own.Apply(it->second, height, Params().MaxLock(), /*strict=*/true, reason)) ++it;
        else it = m_txpool.erase(it);
    }
}

bool ShareChainstateManager::AcceptChainShare(const Share& share, const MmrAccumulator& peaks, const std::vector<uint256>& fruits, const std::vector<ShareTx>& txs, std::string& reason, bool live)
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
    // C4: the transactions (doc/sharechain-transfers.md section 3.3).  Their
    // chain-dependent rules need the window's fruits: skipped where the window
    // runs into a trusted base, as the payout check is.
    if (txs.size() > MAX_TXS_PER_CHAIN_SHARE) {
        reason = "chainshare-too-many-txs";
        return false;
    }
    if (TxsRoot(txs) != share.txs_root) {
        reason = "chainshare-bad-txs-root";
        return false;
    }
    if (!txs.empty()) {
        for (const ShareTx& tx : txs) {
            if (!CheckShareTxForm(tx, Params().ChainId(), reason)) {
                reason = "chainshare-" + reason;
                return false;
            }
        }
        const arith_uint256 window = GetShareWork(WindowBits(share)) * Params().WindowK();
        const FruitLookup lookup = [this](const uint256& h) { return LookupFruit(h); };
        const bool strict = !near_base && !WindowReachesBase(parent, window, Params().WindowShares(), lookup);
        ShareLedger own = ComputeWindowCredits(parent, window, Params().WindowShares(), lookup).ledger;
        // as ComputeWindowCredits will see it: this share's fruits minted first
        for (const uint256& f : fruits) {
            const std::optional<FruitInfo> info = lookup(f);
            if (info) own.Mint(info->miner, share.height, GetShareWork(info->bits));
        }
        own.Expire(share.height);
        for (const ShareTx& tx : txs) {
            if (!own.Apply(tx, share.height, Params().MaxLock(), strict, reason)) {
                reason = "chainshare-" + reason;
                return false;
            }
        }
    }
    auto index = std::make_shared<ShareIndex>();
    index->share = share;
    index->peaks = peaks;
    index->fruits = fruits;
    index->txs = txs;
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
        PruneTxPool();
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

bool ShareChainstateManager::AcceptBaseChainShare(const Share& share, const MmrAccumulator& peaks, const std::vector<uint256>& fruits, const std::vector<ShareTx>& txs, std::string& reason)
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
    if (TxsRoot(txs) != share.txs_root) {
        reason = "chainshare-bad-txs-root";
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
    index->txs = txs;
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
    const arith_uint256 window = GetShareWork(WindowBits(tip->share)) * Params().WindowK();
    arith_uint256 acc = 0;
    size_t walked = 0;
    int floor_height = static_cast<int>(tip->share.height);
    for (const ShareIndex* cur = tip.get(); cur; cur = cur->prev_share.get()) {
        floor_height = static_cast<int>(cur->share.height);
        for (const uint256& f : cur->fruits) {
            auto it = state.fruits.find(f);
            if (it != state.fruits.end()) acc += GetShareWork(it->second.bits);
        }
        if (acc >= window || ++walked >= Params().WindowShares()) break;
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
    share.txs_root = st.txs_root;
    share.miner = miner;
    return share;
}

bool FillNativeShareWitness(Share& share, const CMutableTransaction& cb, const std::vector<uint256>& cb_branch, const CBlockHeader& header, const uint256& chain_id)
{
    if (cb.vin.empty() || cb.vout.empty()) {
        return false;
    }
    share.wit.commit.clear(); // single-entry Merkle map: the branch is empty
    share.wit.cb_hash = cb.GetHash();
    share.wit.cb_version = cb.version;
    share.wit.script_sig.assign(cb.vin[0].scriptSig.begin(), cb.vin[0].scriptSig.end());
    share.wit.sequence = cb.vin[0].nSequence;
    share.wit.total = 0;
    for (const CTxOut& out : cb.vout) {
        share.wit.total += out.GetReferenceValue();
    }
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
        msg.body.txs = cs->second->txs;
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
            return AcceptBaseChainShare(msg.share, msg.body.peaks, msg.body.fruits, msg.body.txs, reason) ? ShareAccept::ACCEPTED : ShareAccept::INVALID;
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
        if (AcceptChainShare(msg.share, msg.body.peaks, msg.body.fruits, msg.body.txs, reason, live)) return ShareAccept::ACCEPTED;
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
    // A share that still waits a day after its template was built belongs to a
    // chain nobody continues (seen live: an abandoned chain's shares rode along in
    // the pool from dump to dump); let it go.
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(m_options.adjusted_time_callback().time_since_epoch()).count();
    if (static_cast<int64_t>(msg.share.wit.nTime) + 24 * 60 * 60 < now) {
        return;
    }
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
constexpr uint64_t SHARECHAIN_DUMP_VERSION = 4; // 4: chain shares carry transactions
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
        m.body.txs = cs->txs;
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
        m.body.txs = cs.txs;
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

namespace {
UniValue ShareTxToJSON(const ShareTx& tx)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", tx.GetHash().GetHex());
    if (tx.type == ShareTx::TRANSFER) {
        o.pushKV("type", "transfer");
        o.pushKV("from", ShareAddressString(tx.from));
        o.pushKV("to", ShareAddressString(tx.to));
        o.pushKV("amount", UintToArith256(tx.amount).getdouble());
        o.pushKV("minheight", (int64_t)tx.min_height);
        o.pushKV("maxheight", (int64_t)tx.max_height);
        if (tx.IsLocked()) {
            o.pushKV("hashlock", HexStr(tx.hashlock));
            o.pushKV("expiry", (int64_t)tx.expiry);
        }
    } else {
        o.pushKV("type", "claim");
        o.pushKV("transfer", tx.transfer_id.GetHex());
    }
    return o;
}

//! 32 bytes of hex in the order written (a lightning payment hash or preimage), not a
//! display-reversed uint256 as ParseHashV reads.
uint256 RawHash32(const UniValue& v, const std::string& name)
{
    const std::string hex = v.get_str();
    if (hex.size() != 64 || !IsHex(hex)) throw JSONRPCError(RPC_INVALID_PARAMETER, name + " must be 32 bytes of hex");
    const std::vector<unsigned char> bytes = ParseHex(hex);
    uint256 out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

void SubmitShareTx(const node::NodeContext& node, const ShareTx& tx) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    std::string reason;
    if (!node.sharechainman->AddShareTx(tx, reason)) {
        throw JSONRPCError(RPC_VERIFY_REJECTED, "share transaction rejected: " + reason);
    }
    if (node.peerman) node.peerman->RelayShareTx(tx, -1);
}
} // namespace

static RPCHelpMan transfershares() {
    return RPCHelpMan{"transfershares",
        "\nTransfer share credits to another address: an amount of the work credited to\n"
        "the key's address in the PPLNS window (newest first, within a height range) is\n"
        "paid to the new owner from the next chain share on (doc/sharechain-transfers.md).\n"
        "With a hashlock it is held for the buyer until claimshares reveals the preimage,\n"
        "or returns after expiry chain shares.\n",
        {
            {"privkey", RPCArg::Type::STR, RPCArg::Optional::NO, "private key (WIF) of the owning address (a bech32 key-hash address)"},
            {"to", RPCArg::Type::STR, RPCArg::Optional::NO, "the new owner, a bech32 address"},
            {"amount", RPCArg::Type::NUM, RPCArg::DefaultHint{"everything in the range"}, "work to transfer, in units of a difficulty-1 share (2^32 hashes); 0 for all"},
            {"maxheight", RPCArg::Type::NUM, RPCArg::DefaultHint{"the share chain tip"}, "credit listed at or below this chain height"},
            {"minheight", RPCArg::Type::NUM, RPCArg::Default{0}, "credit listed at or above this chain height"},
            {"hashlock", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "SHA256 of the preimage the buyer must reveal (32 bytes of hex in byte order, as a lightning payment hash); omit for an unconditional transfer"},
            {"expiry", RPCArg::Type::NUM, RPCArg::DefaultHint{"the chain's maximum lock"}, "with a hashlock: chain shares after the next one until the lock lapses"},
        },
        RPCResult{RPCResult::Type::STR_HEX, "txid", "the share transaction id"},
        RPCExamples{HelpExampleCli("transfershares", "\"cV...\" \"fcrt1q...\"") + HelpExampleRpc("transfershares", "\"cV...\", \"fcrt1q...\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const node::NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.sharechainman) throw JSONRPCError(RPC_MISC_ERROR, "share chain is not enabled");
    const CKey key = DecodeSecret(request.params[0].get_str());
    if (!key.IsValid() || !key.IsCompressed()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key (compressed WIF expected)");
    const CTxDestination dest = DecodeDestination(request.params[1].get_str());
    const std::optional<WitnessUnknown> to = IsValidDestination(dest) ? WitnessUnknownFromDestination(dest) : std::nullopt;
    if (!to) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid destination (a bech32 address is required)");
    const CPubKey pubkey = key.GetPubKey();
    const WitnessV0ShortHash id(0, pubkey);
    ShareTx tx;
    tx.type = ShareTx::TRANSFER;
    tx.from = WitnessUnknown(0u, std::vector<unsigned char>(id.begin(), id.end()));
    tx.to = *to;
    tx.pubkey.assign(pubkey.begin(), pubkey.end());
    LOCK(::cs_main);
    ShareChainstateManager& man = *node.sharechainman;
    const std::shared_ptr<ShareIndex> tip = man.ActiveTip();
    if (!tip) throw JSONRPCError(RPC_MISC_ERROR, "the share chain is empty");
    if (!request.params[2].isNull() && request.params[2].get_real() > 0) {
        tx.amount = ArithToUint256(arith_uint256(static_cast<uint64_t>(request.params[2].get_real())) << 32);
    }
    tx.max_height = request.params[3].isNull() ? tip->share.height : static_cast<uint32_t>(request.params[3].getInt<int>());
    tx.min_height = request.params[4].isNull() ? 0 : static_cast<uint32_t>(request.params[4].getInt<int>());
    if (!request.params[5].isNull()) {
        // raw byte order, as a lightning payment hash is written (not a reversed uint256)
        tx.hashlock = RawHash32(request.params[5], "hashlock");
        const uint32_t after = request.params[6].isNull() ? man.Params().MaxLock() : static_cast<uint32_t>(request.params[6].getInt<int>());
        if (after == 0 || after > man.Params().MaxLock()) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("expiry must be 1..%d chain shares", man.Params().MaxLock()));
        tx.expiry = tip->share.height + 1 + after;
    }
    if (!key.Sign(tx.GetSigHash(man.Params().ChainId()), tx.signature)) throw JSONRPCError(RPC_MISC_ERROR, "signing failed");
    SubmitShareTx(node, tx);
    return tx.GetHash().GetHex();
},
    };
}

static RPCHelpMan claimshares() {
    return RPCHelpMan{"claimshares",
        "\nComplete a hash-locked share transfer by revealing its preimage: the locked fruits\n"
        "become the buyer's from the next chain share on.\n",
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the transfer's share transaction id"},
            {"preimage", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "32 bytes whose SHA256 is the transfer's hashlock (hex, byte order)"},
        },
        RPCResult{RPCResult::Type::STR_HEX, "txid", "the claim's share transaction id"},
        RPCExamples{HelpExampleCli("claimshares", "\"txid\" \"preimage\"") + HelpExampleRpc("claimshares", "\"txid\", \"preimage\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const node::NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.sharechainman) throw JSONRPCError(RPC_MISC_ERROR, "share chain is not enabled");
    ShareTx tx;
    tx.type = ShareTx::CLAIM;
    tx.transfer_id = ParseHashV(request.params[0], "txid");
    tx.preimage = RawHash32(request.params[1], "preimage");
    LOCK(::cs_main);
    SubmitShareTx(node, tx);
    return tx.GetHash().GetHex();
},
    };
}

static RPCHelpMan listsharetxs() {
    return RPCHelpMan{"listsharetxs",
        "\nShare transactions waiting for a chain share, and the hash-locked transfers still\n"
        "pending on the tip.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::ARR, "pool", "transactions the next chain share may carry", {{RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::STR_HEX, "txid", ""},
                {RPCResult::Type::STR, "type", "transfer or claim"},
                {RPCResult::Type::STR, "from", /*optional=*/true, ""},
                {RPCResult::Type::STR, "to", /*optional=*/true, ""},
                {RPCResult::Type::NUM, "amount", /*optional=*/true, "work, in hashes (0: everything in the range)"},
                {RPCResult::Type::NUM, "minheight", /*optional=*/true, ""},
                {RPCResult::Type::NUM, "maxheight", /*optional=*/true, ""},
                {RPCResult::Type::STR_HEX, "hashlock", /*optional=*/true, ""},
                {RPCResult::Type::NUM, "expiry", /*optional=*/true, ""},
                {RPCResult::Type::STR_HEX, "transfer", /*optional=*/true, "the transfer a claim completes"},
            }}}},
            {RPCResult::Type::ARR, "pending", "hash-locked transfers in the window, waiting for a claim", {{RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::STR_HEX, "txid", ""},
                {RPCResult::Type::STR, "type", ""},
                {RPCResult::Type::STR, "from", ""},
                {RPCResult::Type::STR, "to", ""},
                {RPCResult::Type::NUM, "amount", ""},
                {RPCResult::Type::NUM, "minheight", ""},
                {RPCResult::Type::NUM, "maxheight", ""},
                {RPCResult::Type::STR_HEX, "hashlock", ""},
                {RPCResult::Type::NUM, "expiry", ""},
                {RPCResult::Type::NUM, "height", "chain share that carried it"},
                {RPCResult::Type::NUM, "fruits", "fruits' worth it holds"},
                {RPCResult::Type::NUM, "work", "the work it holds (what the buyer is paying for)"},
            }}}},
        }},
        RPCExamples{HelpExampleCli("listsharetxs", "") + HelpExampleRpc("listsharetxs", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const node::NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.sharechainman) throw JSONRPCError(RPC_MISC_ERROR, "share chain is not enabled");
    LOCK(::cs_main);
    const ShareChainstateManager& man = *node.sharechainman;
    UniValue pool(UniValue::VARR);
    for (const ShareTx& tx : man.PoolTxs()) pool.push_back(ShareTxToJSON(tx));
    UniValue pending(UniValue::VARR);
    const std::shared_ptr<ShareIndex> tip = man.ActiveTip();
    if (tip) {
        const ShareLedger own = man.WindowAt(tip.get(), man.WindowBits(tip->share)).ledger;
        for (const auto& item : own.locks) {
            UniValue o = ShareTxToJSON(item.second.tx);
            o.pushKV("height", (int64_t)item.second.height);
            int64_t locked = 0;
            arith_uint256 work = 0;
            for (const auto& b : item.second.held) {
                locked += b.second.fruits;
                work += b.second.work;
            }
            o.pushKV("fruits", locked);
            o.pushKV("work", work.getdouble());
            pending.push_back(o);
        }
    }
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("pool", pool);
    obj.pushKV("pending", pending);
    return obj;
},
    };
}

void RegisterShareChainRPCCommands(CRPCTable& t) {
    static const CRPCCommand commands[]{
        {"mining", &getsharechaininfo},
        {"mining", &transfershares},
        {"mining", &claimshares},
        {"mining", &listsharetxs},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}

// End of File

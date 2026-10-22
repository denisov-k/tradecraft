// Copyright (c) 2020-2023 The Freicoin Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h> // for WitnessUnknown
#include <arith_uint256.h> // for arith_uint256
#include <consensus/amount.h> // for CAmount
#include <common/args.h> // for ArgsManager
#include <consensus/merklerange.h> // for MmrAccumulator
#include <kernel/sharechainstatemanager_opts.h> // for kernel::ShareChainstateManagerOpts
#include <consensus/params.h> // for Consensus::Params
#include <primitives/block.h> // for CBlockHeader, AuxProofOfWork
#include <primitives/transaction.h> // for CMutableTransaction
#include <script/script.h> // for CScript
#include <serialize.h> // for Serialize, Unserialize, VARINT
#include <uint256.h> // for uint256
#include <util/check.h> // for Assert
#include <util/fs.h> // for fs::path
#include <util/sharechaintype.h> // for ShareChainType, ShareChainTypeToString
#include <validation.h> // for cs_main

#include <functional> // for std::function
#include <map> // for std::map
#include <memory> // for std::shared_ptr
#include <optional> // for std::optional
#include <utility> // for std::pair
#include <vector> // for std::vector
#include <string> // for std::string

#include <stdint.h> // for uint32_t

struct ShareChainParams {
    bool IsValid() const { return is_valid; }
    /** Return the share chain type string */
    std::string GetShareChainTypeString() const { return ShareChainTypeToString(m_share_chain_type); }
    /** Return the share chain type */
    ShareChainType GetShareChainType() const { return m_share_chain_type; }
    //! Compact target every share has to meet (fixed for now; retargeting is a later step).
    uint32_t ShareBits() const { return m_share_bits; }
    //! A chain share must meet share target / ChainFactor (M in doc/sharechain-uncles.md).
    uint32_t ChainFactor() const { return m_chain_factor; }
    //! How many chain heights after its anchor a fruit stays listable (R).
    uint32_t Recency() const { return m_recency; }
    //! PPLNS window = WindowK * work(Freicoin aux target).
    uint32_t WindowK() const { return m_window_k; }
    CAmount Dust() const { return m_dust; }
    size_t MaxPayees() const { return m_max_payees; }
    //! Key of the share commitment in the coinbase Merkle hash map.
    const uint256& ChainId() const { return m_chain_id; }
    //! Whether the share target follows the chain (LWMA) or stays at ShareBits().
    bool Retarget() const { return m_retarget; }
    //! A fresh share may build on a Freicoin block at most this many blocks behind the tip.
    int MaxTemplateAge() const { return m_max_template_age; }
    //! Target spacing of shares, seconds.
    int64_t ShareInterval() const { return m_share_interval; }

protected:
    bool is_valid{false};
    ShareChainType m_share_chain_type{ShareChainType::SOLO};
    uint32_t m_share_bits{0x1d00ffff};
    uint32_t m_chain_factor{6};
    uint32_t m_recency{10};
    uint32_t m_window_k{4};
    CAmount m_dust{1000};
    size_t m_max_payees{100};
    uint256 m_chain_id;
    bool m_retarget{true};
    int64_t m_share_interval{10};
    int m_max_template_age{2};
};

/**
 * @brief Registers command-line and config-file options for share chain parameters.
 *
 * @param argsman The command-line and config file option manager.
 */
void SetupShareChainParamsOptions(ArgsManager& argsman);

/**
 * @brief Sets the params returned by ShareParams() to those for the given chain name.
 *
 * @param chain For future expansion.  Must be set to `ShareChainParams::MAIN`.
 * @throws std::runtime_error when the chain is not recognized.
 */
void SelectShareParams(const ShareChainType share_chain);

/**
 * @brief Return the currently selected share chain parameters. This won't
 * change after app startup, except for unit tests.
 *
 * @return const ShareChainParams& The currently selected parameters.
 * @throws std::runtime_error if the share chain hasn't been configured.
 */
const ShareChainParams& ShareParams();

struct ShareWitness {
    //! bit 0: merge-mined share, `aux` is present and the proof of work is
    //! the parent (Bitcoin) header hash reached through it.
    static constexpr uint8_t FLAG_AUX = 0x01;
    uint8_t flags{0};
    //! The auxiliary proof-of-work of the Freicoin header, for merge-mined
    //! shares (FLAG_AUX).  Carries the commit fields, secret, midstate, aux
    //! branch and the parent header, exactly as freicoind received it.
    std::optional<AuxProofOfWork> aux;
    // A share is committed to at the end of the coinbase transaction, which
    // allows for midstate compression in some use cases.  This is not one of
    // those use cases, as the rest of the transaction is required to validate
    // the coinbase rewards, so the whole coinbase transaction is stored.

    //! The share commitment is possibly aggregated with other commitments in
    //! the block using a Merkle hash map structure.  The key is fixed for the
    //! share chain, so we need only store the skip hash and number of skipped
    //! bits for each level.
    std::vector<std::pair<unsigned char, uint256>> commit;

    //! The coinbase transaction of the block, serialized up to the point of
    //! the commitment.
    std::vector<unsigned char> cb1;
    //! The nLockTime and lock_height fields of the coinbase: the only fields
    //! after the commitment (lock_height is serialized for every version-2
    //! transaction, and block templates build version-2 coinbases).
    uint32_t nLockTime{0};
    uint32_t lock_height{0};

    //! The path through the Block's transaction Merkle tree to the coinbase
    //! transaction, which is always the left-most leaf.
    std::vector<uint256> branch;

    //! The bitcoin block header fields.  Note that the Merkle root field is
    //! redundant as it can be computed from the coinbase transaction and
    //! Merkle branch, so it is not included here.
    int32_t nVersion{0};
    uint256 hashPrevBlock;
    //! A unique identifier for the chain this share is a part of.
    uint256 share_chain_path;
    uint32_t nTime{0};
    uint32_t nBits{0};
    uint32_t nNonce{0};

    template<typename Stream>
    void Serialize(Stream &s) const {
        ::Serialize(s, flags);
        if (flags & FLAG_AUX) {
            ::Serialize(s, aux ? *aux : AuxProofOfWork());
        }
        // NB: We use Pieter Wuille's VARINT encoding for vector lengths,
        //     which requires doing our own manual serialization of vectors
        //     and arrays.

        // The commitment proof.  The commit branch size can be between 0 and
        // 256, inclusive, so unfortunately we can't just serialize the length
        // as a single byte.
        ::Serialize(s, VARINT(commit.size()));
        for (const auto& pair : commit) {
            ::Serialize(s, pair.first);  // unsigned char
            ::Serialize(s, pair.second); // uint256
        }
        // The coinbase transaction, minus the commitment (which is inserted
        // after cb1 but before nLockTime).
        ::Serialize(s, VARINT(cb1.size()));
        for (const auto& byte : cb1) {
            ::Serialize(s, byte);
        }
        ::Serialize(s, nLockTime);
        ::Serialize(s, lock_height);
        // The path to the coinbase transaction.
        ::Serialize(s, VARINT(branch.size()));
        for (const auto& hash : branch) {
            ::Serialize(s, hash);
        }
        // The block header fields, with the share chain path instead of the
        // Merkle root (which is computed from the coinbase transaction and
        // Merkle branch).
        ::Serialize(s, nVersion);
        ::Serialize(s, hashPrevBlock);
        ::Serialize(s, share_chain_path);
        ::Serialize(s, nTime);
        ::Serialize(s, nBits);
        ::Serialize(s, nNonce);
    }

    template<typename Stream>
    void Unserialize(Stream &s) {
        ::Unserialize(s, flags);
        aux.reset();
        if (flags & FLAG_AUX) {
            AuxProofOfWork pow;
            ::Unserialize(s, pow);
            aux = std::move(pow);
        }
        // The commitment proof.
        size_t len = 0;
        ::Unserialize(s, VARINT(len));
        commit.resize(len);
        for (auto& pair : commit) {
            ::Unserialize(s, pair.first);  // unsigned char
            ::Unserialize(s, pair.second); // uint256
        }
        // The coinbase transaction, except for the commitment.
        len = 0;
        ::Unserialize(s, VARINT(len));
        cb1.resize(len);
        for (auto& byte : cb1) {
            ::Unserialize(s, byte);
        }
        ::Unserialize(s, nLockTime);
        ::Unserialize(s, lock_height);
        // The path to the coinbase transaction.
        len = 0;
        ::Unserialize(s, VARINT(len));
        branch.resize(len);
        for (auto& hash : branch) {
            ::Unserialize(s, hash);
        }
        // The block header fields, with share_chain_path instead for
        // hashMerkleRoot.
        ::Unserialize(s, nVersion);
        ::Unserialize(s, hashPrevBlock);
        ::Unserialize(s, share_chain_path);
        ::Unserialize(s, nTime);
        ::Unserialize(s, nBits);
        ::Unserialize(s, nNonce);
    }
};

void swap(ShareWitness& lhs, ShareWitness& rhs);

struct Share {
    // First we have the header fields which define the share itself.  These
    // have to do with the share chain.

    //! The version of the share header.  This is used to allow for future
    //! extensions to the share format deployed with miner coordination, much
    //! like versionbits with blocks.
    uint32_t version{0};
    //! The target difficulty of this share.
    uint32_t bits{0};
    //! The height of this share in the share chain.
    uint32_t height{0};
    //! The aggregate work done on the share chain up to but not including
    //! this share.
    uint256 total_work;
    //! The chain share this share hangs from: its anchor.  Null for the
    //! first chain share of a share chain.
    uint256 anchor;
    //! Root of the Merkle Mountain Range of the chain shares up to and
    //! including the anchor.  Only the root is in the header; the peaks
    //! travel with chain shares (ShareIndex::peaks).
    uint256 prev_root;
    //! Merkle root of the sorted list of fruit (share) hashes that this
    //! template records, or null for an empty list.  The list only matters
    //! if the share turns out to meet the chain target, but it has to be
    //! committed in every header because the template is hashed before
    //! anyone knows that (2-for-1 mining).
    uint256 fruits_root;
    //! The address of the miner who submitted this share.  Typically this
    //! wouldn't actually be an unknown witness type, but rather one of the
    //! existing (and understood) witness types in <script/standard.h>.
    //! However (1) we don't actually need to know the type of the witness
    //! here, and (2) we want to support future witness types too.  So we
    //! treat everything as WitnessUnknown.
    WitnessUnknown miner{0u, std::vector<unsigned char>{}};

    // Next we have the witness data, which proves the commitment to the share
    // header within the context of a bitcoin block, as well as the necessary
    // data to verify the share (e.g. the coinbase transaction).

    //! The block commitment data for this share.
    ShareWitness wit;

    SERIALIZE_METHODS(Share, obj) {
        READWRITE(obj.version);
        READWRITE(obj.bits);
        READWRITE(obj.height);
        READWRITE(obj.total_work);
        READWRITE(obj.anchor);
        READWRITE(obj.prev_root);
        READWRITE(obj.fruits_root);
        // WitnessUnknown has no public fields on 28.x: serialize through copies and rebuild on read.
        uint64_t miner_version = obj.miner.GetWitnessVersion();
        std::vector<unsigned char> miner_program = obj.miner.GetWitnessProgram();
        READWRITE(COMPACTSIZE(miner_version));
        READWRITE(miner_program);
        SER_READ(obj, obj.miner = WitnessUnknown(static_cast<unsigned int>(miner_version), miner_program));
        READWRITE(obj.wit);
    }

    //! Hash of the share header fields alone (what the coinbase commits to).
    uint256 GetHeaderHash() const;
    //! The hash the share target applies to: the Freicoin header hash for a
    //! native share, the parent header hash for a merge-mined one.
    uint256 GetPoWHash(const Consensus::Params& params, bool* mutated) const;
    bool IsAux() const { return (wit.flags & ShareWitness::FLAG_AUX) && wit.aux.has_value(); }
    //! The block header for this share.
    CBlockHeader GetBlockHeader(bool* mutated) const;

    //! The hash of the share header.
    uint256 GetHash() const { return GetBlockHeader(nullptr).GetHash(); };
};

void swap(Share& lhs, Share& rhs);

struct ShareIndex;

/** Work of a share with the given compact target: 2^256 / (target + 1), the
 *  same formula as GetBlockProof.  Zero for an invalid target. */
arith_uint256 GetShareWork(uint32_t bits);

/** What ComputeShareCredits needs to know about a listed fruit. */
struct FruitInfo {
    WitnessUnknown miner{0u, std::vector<unsigned char>{}};
    uint32_t bits{0};
};
using FruitLookup = std::function<std::optional<FruitInfo>(const uint256&)>;

/** Work credited to each miner over the PPLNS window: walk the chain shares
 *  backwards from `anchor`, adding the work of every fruit they list, until
 *  the accumulated work reaches `window_work` (the chain share that crosses
 *  the boundary is counted in full).  Fruits the lookup does not know are
 *  skipped; a verifier must make sure they are all known first. */
using ShareCredits = std::map<WitnessUnknown, arith_uint256>;
ShareCredits ComputeShareCredits(const ShareIndex* anchor, const arith_uint256& window_work, const FruitLookup& lookup);

/** Turn credits into coinbase payouts (doc/sharechain.md section 9):
 *  credits are scaled so their sum fits in 190 bits; out_m = floor(total *
 *  credit_m / sum); the flooring remainder goes to `builder`; outputs below
 *  `dust` (other than the builder's) are dropped and the split recomputed
 *  once; if more than `max_payees` remain, the largest credits are kept and
 *  the split recomputed once; the result is in canonical order (version,
 *  program).  The amounts always sum to `total`. */
std::vector<std::pair<WitnessUnknown, CAmount>> ComputeSharePayouts(ShareCredits credits, CAmount total, const WitnessUnknown& builder, CAmount dust, size_t max_payees);

struct ShareIndex {
    //! The share header and witness data.
    Share share;
    //! The previous chain share in the share chain, or nullptr if this is
    //! the genesis chain share.
    std::shared_ptr<const ShareIndex> prev_share;
    //! Chain-share body: the MMR peaks behind share.prev_root, and the
    //! sorted fruit hashes behind share.fruits_root.
    MmrAccumulator peaks;
    std::vector<uint256> fruits;
    //! total_work claimed by the lowest chain share this one descends from
    //! (the chain's base).  Work above it has been verified by this node;
    //! the base's own claim has not, so forks are compared by the difference.
    uint256 base_total_work;

    uint256 GetHash() const { return share.GetHash(); }
};

/** Work of the chain ending in `index` that this node has verified: from
 *  the base (exclusive of its claim) through `index` inclusive. */
arith_uint256 GetVerifiedChainWork(const ShareIndex& index);

/** Body a chain share transmits behind its roots. */
struct ChainShareBody {
    MmrAccumulator peaks;
    std::vector<uint256> fruits;
    SERIALIZE_METHODS(ChainShareBody, obj) { READWRITE(obj.peaks, obj.fruits); }
};

/** A share on the wire and on disk: header + witness, plus the body for chain shares. */
struct ShareMessage {
    Share share;
    bool has_body{false};
    ChainShareBody body;
    SERIALIZE_METHODS(ShareMessage, obj) {
        READWRITE(obj.share);
        uint8_t hb = obj.has_body ? 1 : 0;
        READWRITE(hb);
        SER_READ(obj, obj.has_body = (hb != 0));
        if (obj.has_body) {
            READWRITE(obj.body);
        }
    }
};

enum class ShareAccept { ACCEPTED, DUPLICATE, MISSING, INVALID };

struct ShareChain {
    //! Height of chain[0]: everything below has been pruned.
    int base{0};
    std::vector<std::shared_ptr<ShareIndex>> chain;

    ShareChain() = default;
    ShareChain(const ShareChain&) = delete;
    ShareChain& operator=(const ShareChain&) = delete;

    /** Returns the index entry for the genesis block of this chain, or nullptr if none. */
    std::shared_ptr<ShareIndex> Genesis() const {
        return chain.size() > 0 ? chain[0] : nullptr;
    }

    /** Returns the index entry for the tip of this chain, or nullptr if none. */
    std::shared_ptr<ShareIndex> Tip() const {
        return chain.size() > 0 ? chain.back() : nullptr;
    }

    /** Returns the index entry at a particular height in this chain, or nullptr if no such height exists. */
    std::shared_ptr<ShareIndex> operator[](int height) const {
        if (height < base || height - base >= (int)chain.size())
            return nullptr;
        return chain[height - base];
    }

    /** Efficiently check whether a block is present in this chain. */
    bool Contains(const ShareIndex& index) const
    {
        const std::shared_ptr<ShareIndex> at = (*this)[index.share.height];
        return at && at->GetHash() == index.GetHash();
    }

    /** Find the successor of a block in this chain, or nullptr if the given index is not found or is the tip. */
    std::shared_ptr<ShareIndex> Next(const ShareIndex& index) const
    {
        if (Contains(index))
            return (*this)[index.share.height + 1];
        else
            return nullptr;
    }

    /** Return the maximal height in the chain. Is equal to chain.Tip() ? chain.Tip()->nHeight : -1. */
    int Height() const
    {
        return chain.empty() ? -1 : base + int(chain.size()) - 1;
    }

};

struct ShareChainstate {
    //! The active (most-work) chain of chain shares.
    ShareChain chain;
    //! Every known chain share by hash (active chain and forks).
    std::map<uint256, std::shared_ptr<ShareIndex>> chain_shares;
    //! Every known share (fruit) by hash, chain shares included.
    std::map<uint256, Share> fruits;
};

struct ShareChainstateManager {
    RecursiveMutex& GetMutex() const LOCK_RETURNED(::cs_main) { return ::cs_main; }

    using Options = kernel::ShareChainstateManagerOpts;
    Options m_options;
    std::shared_ptr<ShareChainstate> m_active_chainstate GUARDED_BY(GetMutex()) {nullptr};
    std::shared_ptr<ShareChainstate> m_best_invalid GUARDED_BY(GetMutex()){nullptr};

    explicit ShareChainstateManager(Options options) : m_options(std::move(options)) {
        Assert(m_options.adjusted_time_callback);
    }

    //! The most-work chain.
    ShareChainstate& ActiveChainstate() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) {
        Assert(m_active_chainstate);
        return *m_active_chainstate;
    }
    ShareChain& ActiveChain() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) {
        return ActiveChainstate().chain;
    }
    int ActiveHeight() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) {
        return ActiveChain().Height();
    }
    std::shared_ptr<ShareIndex> ActiveTip() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) {
        return ActiveChain().Tip();
    }

    const ShareChainParams& Params() const { return m_options.sharechainparams; }
    arith_uint256 ShareTarget() const;
    arith_uint256 ChainTarget() const;

    //! Create the (empty) active chainstate.  Call once after construction.
    void Initialize() EXCLUSIVE_LOCKS_REQUIRED(GetMutex());

    //! miner + bits of a known share, for ComputeShareCredits.
    std::optional<FruitInfo> LookupFruit(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());

    //! Everything a template needs from the share chain for a given miner.
    struct TemplateState {
        uint256 anchor;
        uint256 prev_root;
        uint32_t height{0};
        uint256 total_work;
        uint32_t bits{0};
        std::vector<uint256> fruits;
        uint256 fruits_root;
        //! MMR of the chain shares up to and including the anchor: the body a
        //! chain share built from this state transmits.
        MmrAccumulator peaks;
        //! Credits over the window; payouts per builder come from ComputeSharePayouts.
        ShareCredits credits;
        std::vector<std::pair<WitnessUnknown, CAmount>> payouts;
    };
    //! `total` is the coinbase value to split; `frc_bits` the Freicoin aux target
    //! that sizes the PPLNS window.
    TemplateState GetTemplateState(const WitnessUnknown& miner, CAmount total, uint32_t frc_bits) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());

    //! Validate and record a share that met the share target (doc/sharechain-uncles.md A1, A2).
    //! On success the share is in fruits (it may also become a chain share via AcceptChainShare).
    bool AcceptFruit(const Share& share, std::string& reason, bool live = true) EXCLUSIVE_LOCKS_REQUIRED(GetMutex());

    //! Validate and record a share that also met the chain target, with the
    //! body behind its roots (C1, C2); updates the active chain if it has more work (C3).
    bool AcceptChainShare(const Share& share, const MmrAccumulator& peaks, const std::vector<uint256>& fruits, std::string& reason, bool live = true) EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Start an empty chain from a chain share whose anchor we do not have:
    //! the trusted base of a node that joins a running chain (or reloads a
    //! pruned sharechain.dat).  Only the share's own consistency and proof of
    //! work are checked; its total_work is taken as a claim (see
    //! GetVerifiedChainWork).
    bool AcceptBaseChainShare(const Share& share, const MmrAccumulator& peaks, const std::vector<uint256>& fruits, std::string& reason) EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Targets a share with the given compact bits has to meet (its own
    //! bits are fixed by CheckLinkage); ShareTarget()/ChainTarget() are the
    //! easiest the chain allows.
    arith_uint256 ShareTargetFor(uint32_t bits) const;
    arith_uint256 ChainTargetFor(uint32_t bits) const;

    //! Share target a share hanging from `anchor` must meet: ShareBits(), or
    //! the LWMA retarget over the anchor's chain when Params().Retarget().
    uint32_t RequiredShareBits(const ShareIndex* anchor) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());

    //! Validate a share received from a peer or read from disk.  MISSING lists
    //! the hashes (anchor, fruits) we need first; the message is parked in the
    //! orphan pool by the caller via AddOrphan.
    //! `live`: the share was just mined or relayed (template-age rule applies); false for
    //! sync batches and sharechain.dat.
    //! `allow_base`: while the chain is empty, a chain share with an unknown
    //! anchor becomes the trusted base instead of a missing dependency.
    ShareAccept ProcessShareMessage(const ShareMessage& msg, std::vector<uint256>& missing, std::string& reason, bool live = true, bool allow_base = false) EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! A known share as a message (with body for chain shares).
    std::optional<ShareMessage> GetShareMessage(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Locator over the active chain (tip, then exponentially sparser), for getsharechain.
    std::vector<uint256> GetShareLocator() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Shares from the first locator entry on our active chain to the tip, in validation
    //! order (a chain share's non-chain fruits first, then the chain share with body), at most
    //! `max_entries` messages.
    std::vector<ShareMessage> GetShareRange(const std::vector<uint256>& locator, size_t max_entries) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    void AddOrphan(const ShareMessage& msg) EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    std::vector<ShareMessage> TakeOrphans() EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Drop chain shares (and the fruits hanging from them) below the PPLNS window
    //! plus recency and locator margins; keeps memory bounded.
    void Prune() EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Lowest chain height still retained.
    int BaseHeight() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) { return ActiveChain().base; }
    //! Persist / restore the whole share chain state (sharechain.dat).
    bool DumpToFile(const fs::path& path) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    bool LoadFromFile(const fs::path& path) EXCLUSIVE_LOCKS_REQUIRED(GetMutex());

    //! Fruits that a new chain share on top of `tip` may list: known, anchored
    //! within Recency() of the new height, not listed by the last Recency()
    //! ancestors, sorted, unique.
    std::vector<uint256> ListableFruits(const ShareIndex* tip) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());

private:
    //! Linkage checks shared by fruits and chain shares: anchor known (or
    //! null on an empty chain), height, total_work, prev_root.
    bool CheckLinkage(const Share& share, const ShareIndex** anchor_out, std::string& reason) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Merge-mined share: the aux-pow commit root really covers the witness coinbase.
    bool CheckAuxBinding(const Share& share, std::string& reason) const;
    //! Coinbase outputs of the share equal PPLNS(anchor) (A2).
    bool CheckPayouts(const Share& share, const ShareIndex* anchor, std::string& reason) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Whether a chain share at `height` on top of `parent` is within Recency()
    //! of a trusted base, where listed fruits may hang from pruned ancestors.
    bool NearBase(const ShareIndex* parent, uint32_t height) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Rebuild the active chain vector from a tip.
    void SetActiveTip(const std::shared_ptr<ShareIndex>& tip) EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Shares waiting for an anchor or fruits we have not seen yet.
    std::map<uint256, ShareMessage> m_orphans;
};

//! The active share chain manager, for net_processing (set by InitStratumServer).
extern ShareChainstateManager* g_sharechainman;

/** MMR root that a share hanging from `anchor` must carry as prev_root:
 *  the accumulator of all chain shares up to and including the anchor. */
uint256 PrevRootAfter(const ShareIndex* anchor);
/** Merkle root of a fruit list (null for an empty list). */
uint256 FruitsRoot(const std::vector<uint256>& fruits);
/** The commitment output a share template appends last: OP_RETURN <root||id>. */
CScript ShareCommitmentScript(const uint256& commitment_root);
/** Rebuild the full coinbase of a share from its witness (cb1 || root || id || nLockTime || lock_height). */
bool ReconstructShareCoinbase(const Share& share, CMutableTransaction& cb);
/** The share header a template built from `st` for `miner` commits to (native witness, version 0). */
Share ShareFromTemplateState(const ShareChainstateManager::TemplateState& st, const WitnessUnknown& miner);
/** Fill a native witness from the mined coinbase and header; false if the coinbase does not end with a share commitment. */
bool FillNativeShareWitness(Share& share, const CMutableTransaction& cb, const std::vector<uint256>& cb_branch, const CBlockHeader& header, const uint256& chain_id);
/** Attach the auxiliary proof-of-work of a merge-mined submission to a share witness. */
void SetAuxShareWitness(Share& share, const AuxProofOfWork& aux_pow);
/** Witness-program form of a destination, if it has one. */
std::optional<WitnessUnknown> WitnessUnknownFromDestination(const CTxDestination& dest);

// End of File

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
#include <set> // for std::set
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
    //! PPLNS window = WindowK * work(Freicoin aux target), over at most WindowShares() chain shares.
    uint32_t WindowK() const { return m_window_k; }
    //! Upper bound of the window in chain shares (p2pool's CHAIN_LENGTH): a pool
    //! far smaller than the network would otherwise carry weeks of shares.
    size_t WindowShares() const { return m_window_shares; }
    CAmount Dust() const { return m_dust; }
    size_t MaxPayees() const { return m_max_payees; }
    //! Key of the share commitment in the coinbase Merkle hash map.
    const uint256& ChainId() const { return m_chain_id; }
    //! Whether the share target follows the chain (LWMA) or stays at ShareBits().
    bool Retarget() const { return m_retarget; }
    //! A fresh share may build on a Freicoin block at most this many blocks behind the tip.
    int MaxTemplateAge() const { return m_max_template_age; }
    //! A hash-locked transfer lapses at most this many chain shares after its inclusion.
    uint32_t MaxLock() const { return m_max_lock; }
    //! Target spacing of shares, seconds.
    int64_t ShareInterval() const { return m_share_interval; }
    //! Nodes known to run this share chain, connected like -addnode when the share chain
    //! is on (most Freicoin nodes do not run it, so the usual peer search rarely finds one).
    const std::vector<std::string>& Seeds() const { return m_seeds; }

protected:
    bool is_valid{false};
    ShareChainType m_share_chain_type{ShareChainType::SOLO};
    uint32_t m_share_bits{0x1d00ffff};
    uint32_t m_chain_factor{6};
    uint32_t m_recency{10};
    uint32_t m_window_k{4};
    size_t m_window_shares{4320};
    CAmount m_dust{1000};
    size_t m_max_payees{1000};
    uint256 m_chain_id;
    bool m_retarget{true};
    int64_t m_share_interval{10};
    int m_max_template_age{2};
    uint32_t m_max_lock{60};
    std::vector<std::string> m_seeds;
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
    // The coinbase transaction is not carried.  Its outputs are the PPLNS
    // payouts, which every verifier computes itself from the share's anchor,
    // followed by the share commitment; so a share stays the same size no
    // matter how many miners are paid.  Only what a verifier cannot compute
    // travels: the coinbase txid (binds the whole transaction and yields the
    // Merkle root without computing payouts), the input, the transaction
    // version and sequence, the total paid out, and the lock fields.

    //! The share commitment is possibly aggregated with other commitments in
    //! the block using a Merkle hash map structure.  The key is fixed for the
    //! share chain, so we need only store the skip hash and number of skipped
    //! bits for each level.
    std::vector<std::pair<unsigned char, uint256>> commit;

    //! txid of the coinbase transaction: the Merkle leaf, and what the
    //! coinbase rebuilt from the verifier's own payouts must hash to.
    uint256 cb_hash;
    //! The coinbase's version, input script (height, extranonce, miner tag)
    //! and input sequence.
    uint32_t cb_version{2};
    std::vector<unsigned char> script_sig;
    uint32_t sequence{0};
    //! Total value of the coinbase outputs (subsidy plus fees): the amount
    //! PPLNS splits.
    int64_t total{0};
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
        // What the verifier cannot compute of the coinbase transaction.
        ::Serialize(s, cb_hash);
        ::Serialize(s, cb_version);
        ::Serialize(s, VARINT(script_sig.size()));
        for (const auto& byte : script_sig) {
            ::Serialize(s, byte);
        }
        ::Serialize(s, sequence);
        ::Serialize(s, total);
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
        // The coinbase fields.
        ::Unserialize(s, cb_hash);
        ::Unserialize(s, cb_version);
        len = 0;
        ::Unserialize(s, VARINT(len));
        script_sig.resize(len);
        for (auto& byte : script_sig) {
            ::Unserialize(s, byte);
        }
        ::Unserialize(s, sequence);
        ::Unserialize(s, total);
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
    //! Merkle root of the share chain transactions (transfers and claims of
    //! share credits, doc/sharechain-transfers.md) this template carries, or
    //! null.  Like the fruit list they only take effect if the share becomes a
    //! chain share.
    uint256 txs_root;
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
        READWRITE(obj.txs_root);
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

    //! The identity of the share: the hash its target applies to.  For a
    //! native share the Freicoin header hash; for a merge-mined share the
    //! parent (Bitcoin) header hash, because the Freicoin header does not
    //! change between the submissions of one job while the parent does.
    uint256 GetHash() const;
};

void swap(Share& lhs, Share& rhs);

/** A share chain transaction: a transfer of share credits, or the claim that
 *  completes a hash-locked one (doc/sharechain-transfers.md section 3). */
struct ShareTx {
    static constexpr uint8_t TRANSFER = 1;
    static constexpr uint8_t CLAIM = 2;
    uint8_t type{TRANSFER};
    //! transfer: `amount` of the credit `from` holds in the buckets listed at
    //! heights min_height..max_height (newest first; a null amount is all of it)
    //! goes to `to`; with a hashlock it is held for `to` until a claim
    //! reveals the preimage, or returns to `from` when `expiry` passes.
    WitnessUnknown from{0u, std::vector<unsigned char>{}};
    WitnessUnknown to{0u, std::vector<unsigned char>{}};
    uint256 amount;
    uint32_t min_height{0};
    uint32_t max_height{0};
    uint32_t expiry{0};
    uint256 hashlock;
    std::vector<unsigned char> pubkey;    //!< 33 bytes; WitnessV0ShortHash(0, pubkey) is `from`
    std::vector<unsigned char> signature; //!< DER ECDSA over GetSigHash()
    //! claim
    uint256 transfer_id;
    uint256 preimage;

    SERIALIZE_METHODS(ShareTx, obj) {
        READWRITE(obj.type);
        if (obj.type == TRANSFER) {
            uint64_t from_version = obj.from.GetWitnessVersion();
            std::vector<unsigned char> from_program = obj.from.GetWitnessProgram();
            uint64_t to_version = obj.to.GetWitnessVersion();
            std::vector<unsigned char> to_program = obj.to.GetWitnessProgram();
            READWRITE(COMPACTSIZE(from_version), from_program, COMPACTSIZE(to_version), to_program);
            SER_READ(obj, obj.from = WitnessUnknown(static_cast<unsigned int>(from_version), from_program));
            SER_READ(obj, obj.to = WitnessUnknown(static_cast<unsigned int>(to_version), to_program));
            READWRITE(obj.amount, obj.min_height, obj.max_height, obj.expiry, obj.hashlock, obj.pubkey, obj.signature);
        } else {
            READWRITE(obj.transfer_id, obj.preimage);
        }
    }
    bool IsLocked() const { return type == TRANSFER && !hashlock.IsNull(); }
    //! What the owner signs: the transfer without its signature, bound to the chain.
    uint256 GetSigHash(const uint256& chain_id) const;
    //! Transaction id: hash of the whole serialization.
    uint256 GetHash() const;
};

/** Merkle root of a transaction list (null for an empty list). */
uint256 TxsRoot(const std::vector<ShareTx>& txs);
/** Stateless validity of a transaction: field sizes, the key hashing to `from`,
 *  the signature over the chain-bound sighash.  The chain-dependent rules are
 *  ShareLedger::Apply's. */
bool CheckShareTxForm(const ShareTx& tx, const uint256& chain_id, std::string& reason);
/** SHA256 of a claim's preimage, what a hashlock holds. */
uint256 ShareHashLock(const uint256& preimage);

/** The credit ledger of a window: work per address and listing height (a
 *  bucket per height, so credit keeps its age and leaves with the window),
 *  the hash-locked transfers in flight, and the transaction ids seen.  Built
 *  from the window floor to the anchor by minting the listed fruits and
 *  applying the chain shares' transactions in order
 *  (doc/sharechain-transfers.md section 3). */
struct ShareLedger {
    struct Bucket {
        arith_uint256 work;
        int64_t fruits{0};
    };
    using Buckets = std::map<uint32_t, Bucket>; //!< by listing height
    std::map<WitnessUnknown, Buckets> balance;
    struct Lock {
        ShareTx tx;
        uint32_t height{0}; //!< chain share that carried it
        Buckets held;       //!< taken from `from`; paid to `from` until claimed
    };
    std::map<uint256, Lock> locks;
    std::set<uint256> seen; //!< transaction ids applied in this window
    //! Claims carried in this window, by the transfer they completed: what a seller
    //! paid on another chain reads the preimage from (no consensus meaning).
    struct Claimed {
        uint256 claim;     //!< the claim's txid
        uint256 preimage;
        uint32_t height{0};
    };
    std::map<uint256, Claimed> claimed;

    //! Credit a listed fruit to its miner's bucket for `height`.
    void Mint(const WitnessUnknown& miner, uint32_t height, const arith_uint256& work);
    //! Return what the transfers that lapsed before `height` held.
    void Expire(uint32_t height);
    //! Apply a transaction carried by the chain share at `height`.  With
    //! `strict` every chain-dependent rule is checked, a violation fails with
    //! `reason` and the ledger is left untouched; without, it is applied as far
    //! as it goes (near a trusted base, where fruits may be unknown, and when
    //! recomputing a chain that was validated when accepted).
    bool Apply(const ShareTx& tx, uint32_t height, uint32_t max_lock, bool strict, std::string& reason);
    //! Work and fruits credited to each address: its buckets plus what its
    //! locks still hold.
    void Credits(std::map<WitnessUnknown, arith_uint256>& credits, std::map<WitnessUnknown, int64_t>& fruits) const;
    static void Merge(Buckets& into, const Buckets& from);
};

struct ShareIndex;

/** Work of a share with the given compact target: 2^256 / (target + 1), the
 *  same formula as GetBlockProof.  Zero for an invalid target. */
arith_uint256 GetShareWork(uint32_t bits);

/** Address string of a share's miner.  EncodeDestination() refuses a
 *  WitnessUnknown that holds a version-0 program of 20 or 32 bytes (those are
 *  WitnessV0KeyHash / WitnessV0ScriptHash to it), which is exactly what shares
 *  carry: go through the script.  (Found by Vintage Hash.) */
std::string ShareAddressString(const WitnessUnknown& miner);

/** What ComputeShareCredits needs to know about a listed fruit. */
struct FruitInfo {
    WitnessUnknown miner{0u, std::vector<unsigned char>{}};
    uint32_t bits{0};
};
using FruitLookup = std::function<std::optional<FruitInfo>(const uint256&)>;

/** Work credited to each miner over the PPLNS window: walk the chain shares
 *  backwards from `anchor`, adding the work of every fruit they list, until
 *  the accumulated work reaches `window_work` (the chain share that crosses
 *  the boundary is counted in full) or `window_shares` chain shares have
 *  been walked, whichever comes first.  Fruits the lookup does not know are
 *  skipped; a verifier must make sure they are all known first. */
using ShareCredits = std::map<WitnessUnknown, arith_uint256>;
ShareCredits ComputeShareCredits(const ShareIndex* anchor, const arith_uint256& window_work, size_t window_shares, const FruitLookup& lookup);

/** The window in full: credits by address (ComputeShareCredits is this map), the
 *  ledger after the anchor, and figures for RPCs. */
struct WindowCredits {
    ShareCredits credits;
    std::map<WitnessUnknown, int64_t> fruits_by_owner;
    arith_uint256 work;
    size_t chain_shares{0};
    int64_t fruits{0};
    int low_height{-1};
    ShareLedger ledger;
};
WindowCredits ComputeWindowCredits(const ShareIndex* anchor, const arith_uint256& window_work, size_t window_shares, const FruitLookup& lookup);

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
    std::vector<ShareTx> txs;
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
    std::vector<ShareTx> txs;
    SERIALIZE_METHODS(ChainShareBody, obj) { READWRITE(obj.peaks, obj.fruits, obj.txs); }
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
        //! Transactions from the pool that are valid on top of the anchor.
        std::vector<ShareTx> txs;
        uint256 txs_root;
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
    bool AcceptChainShare(const Share& share, const MmrAccumulator& peaks, const std::vector<uint256>& fruits, const std::vector<ShareTx>& txs, std::string& reason, bool live = true) EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Start an empty chain from a chain share whose anchor we do not have:
    //! the trusted base of a node that joins a running chain (or reloads a
    //! pruned sharechain.dat).  Only the share's own consistency and proof of
    //! work are checked; its total_work is taken as a claim (see
    //! GetVerifiedChainWork).
    bool AcceptBaseChainShare(const Share& share, const MmrAccumulator& peaks, const std::vector<uint256>& fruits, const std::vector<ShareTx>& txs, std::string& reason) EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! The PPLNS window of a template or share built on `anchor`, sized by `frc_bits`.
    WindowCredits WindowAt(const ShareIndex* anchor, uint32_t frc_bits) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Validate a transaction against the tip and keep it for the next templates
    //! (doc/sharechain-transfers.md section 3.4).  False with a reason if it is
    //! malformed, invalid on the tip, or already known ("sharetx-duplicate").
    bool AddShareTx(const ShareTx& tx, std::string& reason) EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    std::vector<ShareTx> PoolTxs() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Targets a share with the given compact bits has to meet (its own
    //! bits are fixed by CheckLinkage); ShareTarget()/ChainTarget() are the
    //! easiest the chain allows.
    arith_uint256 ShareTargetFor(uint32_t bits) const;
    arith_uint256 ChainTargetFor(uint32_t bits) const;
    //! Compact target that sizes the PPLNS window for a template or share built on
    //! `prev` (window_bits_callback), or `fallback` when the chain cannot tell.
    uint32_t WindowBitsFor(const uint256& prev, const CBlockHeader& header, uint32_t fallback) const;
    uint32_t WindowBits(const Share& share) const;

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
    //! `cb` is the coinbase CheckPayouts rebuilt.
    bool CheckAuxBinding(const Share& share, const CMutableTransaction& cb, std::string& reason) const;
    //! Coinbase outputs of the share equal PPLNS(anchor) (A2).
    //! On success `cb` holds the rebuilt coinbase, unless the window runs into a
    //! trusted base (then the payouts cannot be computed and `cb` stays empty).
    bool CheckPayouts(const Share& share, const ShareIndex* anchor, std::optional<CMutableTransaction>& cb, std::string& reason) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Whether a chain share at `height` on top of `parent` is within Recency()
    //! of a trusted base, where listed fruits may hang from pruned ancestors.
    bool NearBase(const ShareIndex* parent, uint32_t height) const EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Rebuild the active chain vector from a tip.
    void SetActiveTip(const std::shared_ptr<ShareIndex>& tip) EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
    //! Shares waiting for an anchor or fruits we have not seen yet.
    std::map<uint256, ShareMessage> m_orphans;
    //! True while re-reading our own sharechain.dat: every share in it passed the
    //! full checks when it first arrived, so the expensive payout re-derivation
    //! (a PPLNS window walk per fruit, minutes on a slow PC) is skipped then.
    bool m_loading_own_file{false};
    //! Window credits per (anchor, window) for CheckPayouts: every fruit hung on
    //! the same chain share pays out from the same window, so a sync batch or a
    //! busy share chain recomputes it once per anchor instead of once per fruit.
    struct PayoutWindowCache {
        uint256 anchor;
        uint256 window;
        bool reaches_base{false};
        ShareCredits credits;
    };
    mutable std::vector<PayoutWindowCache> m_payout_window_cache;
    //! Transactions waiting for a chain share to carry them.
    std::map<uint256, ShareTx> m_txpool;
    //! Drop pool transactions the current tip no longer accepts.
    void PruneTxPool() EXCLUSIVE_LOCKS_REQUIRED(GetMutex());
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
/** Build the coinbase a share's template must have had: the witness fields,
 *  the given payouts, and the share commitment last.  Its txid must equal
 *  the witness cb_hash. */
bool BuildShareCoinbase(const Share& share, const std::vector<std::pair<WitnessUnknown, CAmount>>& payouts, CMutableTransaction& cb);
/** The share header a template built from `st` for `miner` commits to (native witness, version 0). */
Share ShareFromTemplateState(const ShareChainstateManager::TemplateState& st, const WitnessUnknown& miner);
/** Fill a native witness from the mined coinbase and header; false if the coinbase does not end with a share commitment. */
bool FillNativeShareWitness(Share& share, const CMutableTransaction& cb, const std::vector<uint256>& cb_branch, const CBlockHeader& header, const uint256& chain_id);
/** Attach the auxiliary proof-of-work of a merge-mined submission to a share witness. */
void SetAuxShareWitness(Share& share, const AuxProofOfWork& aux_pow);
/** Witness-program form of a destination, if it has one. */
std::optional<WitnessUnknown> WitnessUnknownFromDestination(const CTxDestination& dest);

// End of File

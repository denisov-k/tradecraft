// Copyright (c) 2020-2023 The Freicoin Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sharechain.h>

#include <consensus/merkle.h> // for ComputeMerkleMapRootFromBranch, ComputeMerkleRootFromBranch
#include <hash.h> // for CHash256, HashWriter
#include <primitives/block.h> // for CBlockHeader
#include <rpc/server.h> // for CRPCTable
#include <streams.h> // for DataStream
#include <span.h> // for Span, MakeUCharSpan
#include <util/check.h> // for Assert

#include <algorithm> // for std::sort
#include <array> // for std::array
#include <utility> // for std::swap

void SetupShareChainParamsOptions(ArgsManager& argsman) {
    argsman.AddArg("-sharechain=<name>", "Use the share chain <name> (default: main). Allowed values: solo or main", ArgsManager::ALLOW_ANY, OptionsCategory::STRATUM);
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
    swap(lhs.commit, rhs.commit);
    swap(lhs.cb1, rhs.cb1);
    swap(lhs.nLockTime, rhs.nLockTime);
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

CBlockHeader Share::GetBlockHeader(bool *mutated) const {
    if (mutated) {
        *mutated = false;
    }

    // Compute the hash of the share header.
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
    uint256 hash = ss.GetHash();

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
    return blkhdr;
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

static RPCHelpMan getsharechaininfo() {
    return RPCHelpMan{"getsharechaininfo",
        "\nReturns an object containing various state info regarding the share chain.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::BOOL, "enabled", "whether the share chain service is enabled"},
            {RPCResult::Type::STR, "network", /*optional=*/true, "which share chain the mining service is using"},
        }},
        RPCExamples{
            HelpExampleCli("getstratuminfo", "")
            + HelpExampleRpc("getstratuminfo", "")
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

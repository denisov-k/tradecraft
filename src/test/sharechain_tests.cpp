// Copyright (c) 2026 The Freicoin Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sharechain.h>

#include <addresstype.h>
#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/merklerange.h>
#include <crypto/common.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/data/sharechain_pplns.json.h>
#include <test/util/json.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/sharechaintype.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include <map>
#include <memory>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(sharechain_tests, BasicTestingSetup)

namespace {
WitnessUnknown MinerFromHex(const std::string& hex)
{
    return WitnessUnknown(0u, ParseHex(hex));
}

arith_uint256 ParseWork(const std::string& hex_with_prefix)
{
    // "0x..." as written by the Python reference
    return UintToArith256(uint256S(hex_with_prefix.substr(2)));
}
} // namespace

BOOST_AUTO_TEST_CASE(share_work_matches_block_proof)
{
    // Bitcoin genesis difficulty: 2^256 / (0x00000000ffff << 208 + 1) == 0x100010001
    BOOST_CHECK_EQUAL(GetShareWork(0x1d00ffff).GetLow64(), 0x100010001ULL);
    BOOST_CHECK_EQUAL(GetShareWork(0).GetLow64(), 0ULL);
    // a larger target means less work
    BOOST_CHECK(GetShareWork(0x1d00ffff) > GetShareWork(0x1e00ffff));
}

BOOST_AUTO_TEST_CASE(pplns_vectors)
{
    // Vectors from contrib/sharechain/pplns_fruits.py (doc/sharechain-uncles.md section 4).
    UniValue tests = read_json(json_tests::sharechain_pplns);
    BOOST_REQUIRE(tests.isArray());
    BOOST_REQUIRE(tests.size() == 5);

    for (unsigned int idx = 0; idx < tests.size(); ++idx) {
        const UniValue& test = tests[idx];
        const UniValue& chain = test["chain"];
        BOOST_REQUIRE(chain.isArray());

        // Build the chain of chain shares with synthetic fruit hashes.
        std::map<uint256, FruitInfo> fruit_info;
        std::vector<std::shared_ptr<ShareIndex>> index;
        for (unsigned int i = 0; i < chain.size(); ++i) {
            auto entry = std::make_shared<ShareIndex>();
            entry->share.miner = MinerFromHex(chain[i]["miner"].get_str());
            entry->share.bits = static_cast<uint32_t>(chain[i]["bits"].getInt<int64_t>());
            entry->share.height = i;
            if (i > 0) entry->prev_share = index[i - 1];
            const UniValue& fruits = chain[i]["fruits"];
            for (unsigned int j = 0; j < fruits.size(); ++j) {
                uint256 h;
                // deterministic synthetic hash: case, chain index, fruit index
                WriteLE32(h.begin(), idx);
                WriteLE32(h.begin() + 4, i);
                WriteLE32(h.begin() + 8, j);
                fruit_info[h] = FruitInfo{MinerFromHex(fruits[j]["miner"].get_str()), static_cast<uint32_t>(fruits[j]["bits"].getInt<int64_t>())};
                entry->fruits.push_back(h);
            }
            index.push_back(entry);
        }

        const FruitLookup lookup = [&fruit_info](const uint256& h) -> std::optional<FruitInfo> {
            auto it = fruit_info.find(h);
            if (it == fruit_info.end()) return std::nullopt;
            return it->second;
        };

        const arith_uint256 window = ParseWork(test["window_work"].get_str());
        const size_t window_shares = static_cast<size_t>(test["window_shares"].getInt<int64_t>());
        const CAmount total = test["total"].getInt<int64_t>();
        const WitnessUnknown builder = MinerFromHex(test["builder"].get_str());
        const CAmount dust = test["dust"].getInt<int64_t>();
        const size_t max_payees = static_cast<size_t>(test["max_payees"].getInt<int64_t>());

        ShareCredits credits = ComputeShareCredits(index.back().get(), window, window_shares, lookup);
        const auto payouts = ComputeSharePayouts(std::move(credits), total, builder, dust, max_payees);

        const UniValue& expected = test["outputs"];
        BOOST_CHECK_EQUAL(payouts.size(), expected.size());
        CAmount paid = 0;
        for (unsigned int k = 0; k < expected.size() && k < payouts.size(); ++k) {
            BOOST_CHECK_EQUAL(HexStr(payouts[k].first.GetWitnessProgram()), expected[k][0].get_str());
            BOOST_CHECK_EQUAL(payouts[k].second, expected[k][1].getInt<int64_t>());
            paid += payouts[k].second;
        }
        BOOST_CHECK_EQUAL(paid, total);
    }
}

BOOST_AUTO_TEST_CASE(pplns_edge_cases)
{
    const WitnessUnknown builder = MinerFromHex("0101010101010101010101010101010101010101");
    // No credits at all: the builder gets everything (solo / genesis case).
    auto outs = ComputeSharePayouts(ShareCredits{}, 1000, builder, 10, 100);
    BOOST_REQUIRE_EQUAL(outs.size(), 1U);
    BOOST_CHECK(outs[0].first == builder);
    BOOST_CHECK_EQUAL(outs[0].second, 1000);

    // Dust below the threshold is dropped and the sum is preserved.
    ShareCredits credits;
    credits[MinerFromHex("0202020202020202020202020202020202020202")] = 1000;
    credits[MinerFromHex("0303030303030303030303030303030303030303")] = 1;
    outs = ComputeSharePayouts(credits, 1000, builder, 10, 100);
    CAmount sum = 0;
    for (const auto& o : outs) sum += o.second;
    BOOST_CHECK_EQUAL(sum, 1000);
    BOOST_CHECK_EQUAL(outs.size(), 1U); // miner 02 only: no remainder, builder not credited
}

namespace {
uint256 FakeHash(uint64_t n)
{
    return ArithToUint256(arith_uint256(n));
}

/** A share chain manager on regtest share parameters with a synthetic view of
 *  the Freicoin chain: blocks 90..100 are known, the tip is 100, and every
 *  block requires the regtest difficulty. */
struct ShareChainRegtestSetup : public BasicTestingSetup {
    std::map<uint256, int> m_blocks;
    int m_tip_height{100};
    uint32_t m_required_bits{0x207fffff};
    int64_t m_now{1700000000};
    std::unique_ptr<ShareChainstateManager> m_man;

    ShareChainRegtestSetup() : BasicTestingSetup(ChainType::REGTEST)
    {
        SelectShareParams(ShareChainType::REGTEST);
        for (int h = 90; h <= 100; ++h) {
            m_blocks[BlockHash(h)] = h;
        }
        m_man = std::make_unique<ShareChainstateManager>(MakeOptions());
        LOCK(::cs_main);
        m_man->Initialize();
    }

    static uint256 BlockHash(int height) { return FakeHash(0x1000 + height); }

    ShareChainstateManager::Options MakeOptions()
    {
        return ShareChainstateManager::Options{
            .chainparams = Params(),
            .sharechainparams = ShareParams(),
            .adjusted_time_callback = [this]() { return NodeClock::time_point{std::chrono::seconds{m_now}}; },
            .block_height_callback = [this](const uint256& hash) -> std::optional<int> {
                auto it = m_blocks.find(hash);
                if (it == m_blocks.end()) return std::nullopt;
                return it->second;
            },
            .tip_height_callback = [this]() { return m_tip_height; },
            .required_bits_callback = [this](const uint256&, const CBlockHeader&) -> std::optional<uint32_t> { return m_required_bits; },
            .window_bits_callback = [this](const uint256&, const CBlockHeader&) -> std::optional<uint32_t> { return m_required_bits; },
        };
    }

    struct Mined {
        Share share;
        MmrAccumulator peaks;
        std::vector<uint256> fruits;
    };
    enum class Kind { FRUIT, CHAIN };

    /** Build the template the manager would hand `miner`, assemble its coinbase
     *  (payouts + commitment) and grind a header until it is a fruit only
     *  (between chain and share target) or a chain share.  `tweak` edits the
     *  coinbase before the commitment, `fruits` overrides the fruit list,
     *  `header_tweak` edits the share header; all three produce invalid shares. */
    Mined Mine(const WitnessUnknown& miner, Kind kind, int prev_height = 100, uint32_t ntime = 0, uint32_t nbits = 0x207fffff,
               const std::function<void(CMutableTransaction&)>& tweak = nullptr,
               const std::optional<std::vector<uint256>>& fruits = std::nullopt,
               const std::function<void(Share&)>& header_tweak = nullptr,
               const std::optional<ShareChainstateManager::TemplateState>& template_state = std::nullopt) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        ShareChainstateManager::TemplateState st = template_state ? *template_state : m_man->GetTemplateState(miner, 50 * COIN, nbits);
        if (fruits) {
            st.fruits = *fruits;
            st.fruits_root = FruitsRoot(st.fruits);
        }
        Share share = ShareFromTemplateState(st, miner);
        if (header_tweak) header_tweak(share);
        CMutableTransaction cb;
        cb.version = CTransaction::CURRENT_VERSION;
        cb.vin.emplace_back(COutPoint(), CScript() << OP_0 << OP_0);
        for (const auto& payout : st.payouts) {
            cb.vout.emplace_back(payout.second, GetScriptForDestination(payout.first));
        }
        if (tweak) tweak(cb);
        const uint256 root = ComputeMerkleMapRootFromBranch(share.GetHeaderHash(), {}, ShareParams().ChainId(), nullptr);
        cb.vout.emplace_back(0, ShareCommitmentScript(root));
        CBlockHeader header;
        header.nVersion = 0x20000000;
        header.hashPrevBlock = BlockHash(prev_height);
        header.hashMerkleRoot = cb.GetHash();
        header.nTime = ntime ? ntime : static_cast<uint32_t>(m_now);
        header.nBits = nbits;
        const arith_uint256 share_target = m_man->ShareTargetFor(share.bits);
        const arith_uint256 chain_target = m_man->ChainTargetFor(share.bits);
        for (header.nNonce = 0;; ++header.nNonce) {
            const arith_uint256 h = UintToArith256(header.GetHash());
            const bool ok = kind == Kind::CHAIN ? h <= chain_target : (h > chain_target && h <= share_target);
            if (ok) break;
        }
        BOOST_REQUIRE(FillNativeShareWitness(share, cb, {}, header, ShareParams().ChainId()));
        return Mined{share, st.peaks, st.fruits};
    }

    bool Fruit(const Mined& m, std::string& reason, bool live = true) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        return m_man->AcceptFruit(m.share, reason, live);
    }
    bool Chain(const Mined& m, std::string& reason) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        return m_man->AcceptChainShare(m.share, m.peaks, m.fruits, reason);
    }
};

const WitnessUnknown MINER_A = WitnessUnknown(0u, ParseHex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
const WitnessUnknown MINER_B = WitnessUnknown(0u, ParseHex("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
} // namespace

BOOST_FIXTURE_TEST_CASE(sharechain_accept_and_pay, ShareChainRegtestSetup)
{
    LOCK(::cs_main);
    std::string reason;

    // An empty chain pays the builder everything.
    auto st = m_man->GetTemplateState(MINER_A, COIN, 0x207fffff);
    BOOST_CHECK_EQUAL(st.height, 0U);
    BOOST_CHECK(st.anchor.IsNull());
    BOOST_REQUIRE_EQUAL(st.payouts.size(), 1U);
    BOOST_CHECK(st.payouts[0].first == MINER_A);
    BOOST_CHECK_EQUAL(st.payouts[0].second, COIN);

    // A fruit that is not a chain share.
    const Mined f0 = Mine(MINER_A, Kind::FRUIT);
    BOOST_CHECK_MESSAGE(Fruit(f0, reason), reason);
    BOOST_CHECK(!Fruit(f0, reason));
    BOOST_CHECK_EQUAL(reason, "share-duplicate");
    BOOST_CHECK(!Chain(f0, reason));
    BOOST_CHECK_EQUAL(reason, "chainshare-high-hash");
    BOOST_CHECK_EQUAL(m_man->ActiveHeight(), -1);

    // The first chain share lists it.
    const Mined c0 = Mine(MINER_A, Kind::CHAIN);
    BOOST_REQUIRE_EQUAL(c0.fruits.size(), 1U);
    BOOST_CHECK(c0.fruits[0] == f0.share.GetHash());
    BOOST_CHECK_MESSAGE(Chain(c0, reason), reason);
    BOOST_CHECK_EQUAL(m_man->ActiveHeight(), 0);

    // B builds on it: all credit so far is A's, B (the builder) gets only a zero remainder.
    st = m_man->GetTemplateState(MINER_B, COIN, 0x207fffff);
    BOOST_CHECK_EQUAL(st.height, 1U);
    BOOST_CHECK(st.anchor == c0.share.GetHash());
    BOOST_REQUIRE_EQUAL(st.payouts.size(), 1U);
    BOOST_CHECK(st.payouts[0].first == MINER_A);
    BOOST_REQUIRE_EQUAL(st.fruits.size(), 1U); // c0 itself is a listable fruit now
    BOOST_CHECK(st.fruits[0] == c0.share.GetHash());

    const Mined f1 = Mine(MINER_B, Kind::FRUIT);
    BOOST_CHECK_MESSAGE(Fruit(f1, reason), reason);
    const Mined c1 = Mine(MINER_B, Kind::CHAIN);
    BOOST_CHECK_EQUAL(c1.fruits.size(), 2U); // c0 and f1
    BOOST_CHECK_MESSAGE(Chain(c1, reason), reason);
    BOOST_CHECK_EQUAL(m_man->ActiveHeight(), 1);

    // Credits: f0 and c0 for A, f1 for B, equal work each: 2/3 and 1/3.
    st = m_man->GetTemplateState(MINER_A, 3 * COIN, 0x207fffff);
    BOOST_REQUIRE_EQUAL(st.payouts.size(), 2U);
    BOOST_CHECK(st.payouts[0].first == MINER_A);
    BOOST_CHECK_EQUAL(st.payouts[0].second, 2 * COIN);
    BOOST_CHECK(st.payouts[1].first == MINER_B);
    BOOST_CHECK_EQUAL(st.payouts[1].second, COIN);

    // A chain share may not list a fruit a recent ancestor already listed.
    const Mined relist = Mine(MINER_A, Kind::CHAIN, 100, 0, 0x207fffff, nullptr, std::vector<uint256>{f0.share.GetHash()});
    BOOST_CHECK(!Chain(relist, reason));
    BOOST_CHECK_EQUAL(reason, "chainshare-fruit-relisted");
    BOOST_CHECK(m_man->LookupFruit(relist.share.GetHash()).has_value()); // but it is a valid fruit

    // The peaks must be the accumulator behind prev_root.
    const Mined c2 = Mine(MINER_A, Kind::CHAIN);
    BOOST_CHECK(!m_man->AcceptChainShare(c2.share, MmrAccumulator(), c2.fruits, reason));
    BOOST_CHECK_EQUAL(reason, "chainshare-bad-peaks");
    BOOST_CHECK_MESSAGE(Chain(c2, reason), reason);
    BOOST_CHECK_EQUAL(m_man->ActiveHeight(), 2);
    BOOST_CHECK(m_man->ActiveTip()->GetHash() == c2.share.GetHash());
}

BOOST_FIXTURE_TEST_CASE(sharechain_empty_chain_fruits_after_genesis, ShareChainRegtestSetup)
{
    // Miners keep working on the empty-chain template for a while after the first
    // chain share: those fruits (null anchor) are valid while the chain is younger
    // than R, and the next chain share lists them.  A reload must accept them too.
    LOCK(::cs_main);
    std::string reason;
    const auto empty_template = m_man->GetTemplateState(MINER_B, 50 * COIN, 0x207fffff);
    const Mined c0 = Mine(MINER_A, Kind::CHAIN);
    BOOST_REQUIRE_MESSAGE(Chain(c0, reason), reason);
    const Mined late = Mine(MINER_B, Kind::FRUIT, 100, 0, 0x207fffff, nullptr, std::nullopt, nullptr, empty_template);
    BOOST_CHECK(late.share.anchor.IsNull());
    BOOST_CHECK_MESSAGE(Fruit(late, reason), reason);
    const Mined c1 = Mine(MINER_A, Kind::CHAIN);
    BOOST_CHECK(std::find(c1.fruits.begin(), c1.fruits.end(), late.share.GetHash()) != c1.fruits.end());
    BOOST_REQUIRE_MESSAGE(Chain(c1, reason), reason);
    // Reload in file order: genesis, then the late fruit, then c1.
    const fs::path path = m_path_root / "sharechain-young.dat";
    BOOST_REQUIRE(m_man->DumpToFile(path));
    ShareChainstateManager reloaded(MakeOptions());
    reloaded.Initialize();
    BOOST_REQUIRE(reloaded.LoadFromFile(path));
    BOOST_CHECK_EQUAL(reloaded.ActiveHeight(), 1);
    BOOST_CHECK_EQUAL(reloaded.ActiveChainstate().fruits.size(), m_man->ActiveChainstate().fruits.size());
    // Once the chain is R deep, a null-anchored fruit is stale.
    for (uint32_t i = 2; i < ShareParams().Recency() + 1; ++i) {
        const Mined c = Mine(MINER_A, Kind::CHAIN, 100, static_cast<uint32_t>(m_now + 10 * i));
        BOOST_REQUIRE_MESSAGE(Chain(c, reason), reason);
    }
    BOOST_CHECK_GE(m_man->ActiveHeight(), static_cast<int>(ShareParams().Recency()));
    const Mined stale = Mine(MINER_B, Kind::FRUIT, 100, static_cast<uint32_t>(m_now + 200), 0x207fffff, nullptr, std::nullopt, nullptr, empty_template);
    BOOST_CHECK(!Fruit(stale, reason));
    BOOST_CHECK_EQUAL(reason, "share-anchor-null");
}

BOOST_FIXTURE_TEST_CASE(sharechain_rejects_invalid_shares, ShareChainRegtestSetup)
{
    LOCK(::cs_main);
    std::string reason;
    const Mined c0 = Mine(MINER_A, Kind::CHAIN);
    BOOST_REQUIRE_MESSAGE(Chain(c0, reason), reason);

    // Payout redirected to another address (same amounts).
    Mined m = Mine(MINER_B, Kind::FRUIT, 100, 0, 0x207fffff, [](CMutableTransaction& cb) { cb.vout[0].scriptPubKey = GetScriptForDestination(MINER_A); });
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-bad-payout");

    // An extra output that carries value changes the split the total implies.
    m = Mine(MINER_B, Kind::FRUIT, 100, 0, 0x207fffff, [](CMutableTransaction& cb) { cb.vout.emplace_back(1, GetScriptForDestination(MINER_A)); });
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-bad-payout");

    // The template's nBits must be the Freicoin difficulty (it sizes the window).
    m = Mine(MINER_B, Kind::FRUIT, 100, 0, 0x207ffffe);
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-bad-frc-bits");

    // Built on a block we do not know.
    m = Mine(MINER_B, Kind::FRUIT, 50);
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-unknown-prev-block");

    // Built on a block deeper than MaxTemplateAge (2) below the tip: stale when live, fine from a batch.
    m = Mine(MINER_B, Kind::FRUIT, 97);
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-stale-template");
    BOOST_CHECK_MESSAGE(Fruit(m, reason, /*live=*/false), reason);
    m = Mine(MINER_B, Kind::FRUIT, 98);
    BOOST_CHECK_MESSAGE(Fruit(m, reason), reason);

    // Timestamps.
    m = Mine(MINER_B, Kind::FRUIT, 100, static_cast<uint32_t>(m_now + 3 * 60 * 60));
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-time-too-new");
    m = Mine(MINER_B, Kind::FRUIT, 100, static_cast<uint32_t>(m_now - 601));
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-time-too-old");

    // Header fields the chain prescribes.
    m = Mine(MINER_B, Kind::FRUIT, 100, 0, 0x207fffff, nullptr, std::nullopt, [](Share& s) { s.bits = 0x207ffffe; });
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-bad-bits");
    m = Mine(MINER_B, Kind::FRUIT, 100, 0, 0x207fffff, nullptr, std::nullopt, [](Share& s) { s.anchor = FakeHash(77); });
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-anchor-unknown");
    m = Mine(MINER_B, Kind::FRUIT, 100, 0, 0x207fffff, nullptr, std::nullopt, [](Share& s) { s.height = 5; });
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-bad-height");
    m = Mine(MINER_B, Kind::FRUIT);
    m.share.wit.share_chain_path = FakeHash(7);
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-bad-chain-id");

    // And the honest one is still fine.
    m = Mine(MINER_B, Kind::FRUIT);
    BOOST_CHECK_MESSAGE(Fruit(m, reason), reason);
}

BOOST_FIXTURE_TEST_CASE(sharechain_prune_sync_and_reload, ShareChainRegtestSetup)
{
    std::string reason;
    const int kChainShares = 90;
    {
        LOCK(::cs_main);
        for (int i = 0; i < kChainShares; ++i) {
            const uint32_t t = static_cast<uint32_t>(m_now + 10 * i);
            const Mined f = Mine(i % 2 ? MINER_A : MINER_B, Kind::FRUIT, 100, t);
            BOOST_REQUIRE_MESSAGE(Fruit(f, reason), reason);
            const Mined c = Mine(i % 2 ? MINER_B : MINER_A, Kind::CHAIN, 100, t + 5);
            BOOST_REQUIRE_MESSAGE(Chain(c, reason), reason);
        }
        BOOST_CHECK_EQUAL(m_man->ActiveHeight(), kChainShares - 1);
        // Pruned: the chain starts above genesis, nothing below the base is kept.
        const int base = m_man->BaseHeight();
        BOOST_CHECK_GT(base, 0);
        BOOST_CHECK_LT(base, kChainShares - 20);
        const ShareChainstate& state = m_man->ActiveChainstate();
        BOOST_CHECK_EQUAL(state.chain_shares.size(), static_cast<size_t>(kChainShares - base));
        for (const auto& item : state.fruits) {
            const Share& f = item.second;
            BOOST_CHECK(!f.anchor.IsNull());
            BOOST_CHECK(state.chain_shares.count(f.anchor) || state.chain_shares.count(item.first));
        }
        // The window is intact: both miners are paid.
        const auto st = m_man->GetTemplateState(MINER_A, 2 * COIN, 0x207fffff);
        BOOST_CHECK_EQUAL(st.payouts.size(), 2U);
    }

    // A fresh node syncs the pruned chain from this one: the first chain share it gets is its base.
    uint256 tip;
    size_t chain_shares = 0, fruits = 0;
    int base_height = 0;
    std::vector<ShareMessage> batch;
    {
        LOCK(::cs_main);
        tip = m_man->ActiveTip()->GetHash();
        chain_shares = m_man->ActiveChainstate().chain_shares.size();
        fruits = m_man->ActiveChainstate().fruits.size();
        base_height = m_man->BaseHeight();
        batch = m_man->GetShareRange({}, 500);
    }
    BOOST_CHECK_GT(batch.size(), chain_shares);
    {
        ShareChainstateManager peer(MakeOptions());
        LOCK(::cs_main);
        peer.Initialize();
        size_t accepted = 0, invalid = 0, missing = 0;
        for (const ShareMessage& m : batch) {
            std::vector<uint256> deps;
            switch (peer.ProcessShareMessage(m, deps, reason, /*live=*/false, /*allow_base=*/true)) {
            case ShareAccept::ACCEPTED: ++accepted; break;
            case ShareAccept::INVALID: ++invalid; BOOST_TEST_MESSAGE("invalid: " + reason); break;
            case ShareAccept::MISSING: ++missing; break;
            case ShareAccept::DUPLICATE: break;
            }
        }
        BOOST_CHECK_EQUAL(invalid, 0U);
        BOOST_CHECK_EQUAL(missing, 0U);
        BOOST_CHECK_EQUAL(accepted, batch.size());
        BOOST_REQUIRE(peer.ActiveTip());
        BOOST_CHECK(peer.ActiveTip()->GetHash() == tip);
        BOOST_CHECK_EQUAL(peer.BaseHeight(), base_height);
        BOOST_CHECK_EQUAL(peer.ActiveChainstate().chain_shares.size(), chain_shares);
        BOOST_CHECK_EQUAL(peer.ActiveChainstate().fruits.size(), fruits);
        // The peer pays the same way.
        const auto st = peer.GetTemplateState(MINER_A, 2 * COIN, 0x207fffff);
        BOOST_CHECK_EQUAL(st.payouts.size(), 2U);
        // A share below the base is neither needed nor an orphan.
        BOOST_CHECK(!batch.empty());
        ShareMessage below = batch.front();
        below.share.height = 0;
        below.share.anchor = FakeHash(99);
        std::vector<uint256> deps;
        BOOST_CHECK(peer.ProcessShareMessage(below, deps, reason, /*live=*/false) == ShareAccept::INVALID);
        BOOST_CHECK_EQUAL(reason, "share-below-base");
    }

    // The pruned dump reloads into the same chain.
    const fs::path path = m_path_root / "sharechain.dat";
    {
        LOCK(::cs_main);
        BOOST_REQUIRE(m_man->DumpToFile(path));
    }
    {
        ShareChainstateManager reloaded(MakeOptions());
        LOCK(::cs_main);
        reloaded.Initialize();
        BOOST_REQUIRE(reloaded.LoadFromFile(path));
        BOOST_REQUIRE(reloaded.ActiveTip());
        BOOST_CHECK(reloaded.ActiveTip()->GetHash() == tip);
        BOOST_CHECK_EQUAL(reloaded.BaseHeight(), base_height);
        BOOST_CHECK_EQUAL(reloaded.ActiveChainstate().chain_shares.size(), chain_shares);
        BOOST_CHECK_EQUAL(reloaded.ActiveChainstate().fruits.size(), fruits);
    }
}

BOOST_FIXTURE_TEST_CASE(sharechain_verified_work_beats_claimed_work, ShareChainRegtestSetup)
{
    // Two forks from the same base compare by work above the base; a fork whose base
    // merely claims more total_work does not win by the claim alone.
    LOCK(::cs_main);
    std::string reason;
    const Mined c0 = Mine(MINER_A, Kind::CHAIN);
    BOOST_REQUIRE_MESSAGE(Chain(c0, reason), reason);
    const Mined c1 = Mine(MINER_A, Kind::CHAIN, 100, static_cast<uint32_t>(m_now + 10));
    BOOST_REQUIRE_MESSAGE(Chain(c1, reason), reason);
    const auto tip = m_man->ActiveTip();
    BOOST_CHECK_EQUAL(GetVerifiedChainWork(*tip).GetLow64(), (GetShareWork(c0.share.bits) + GetShareWork(c1.share.bits)).GetLow64());

    // A fresh manager started from c1 as a trusted base counts only the work above it.
    ShareChainstateManager peer(MakeOptions());
    peer.Initialize();
    BOOST_REQUIRE_MESSAGE(peer.AcceptBaseChainShare(c1.share, c1.peaks, c1.fruits, reason), reason);
    BOOST_CHECK_EQUAL(peer.BaseHeight(), 1);
    BOOST_CHECK_EQUAL(GetVerifiedChainWork(*peer.ActiveTip()).GetLow64(), GetShareWork(c1.share.bits).GetLow64());
    BOOST_CHECK(!peer.AcceptBaseChainShare(c0.share, c0.peaks, c0.fruits, reason));
    BOOST_CHECK_EQUAL(reason, "base-chain-not-empty");
}

BOOST_FIXTURE_TEST_CASE(sharechain_window_share_cap, ShareChainRegtestSetup)
{
    LOCK(::cs_main);
    std::string reason;
    // A Freicoin target far harder than any regtest share: by work alone the window
    // would hold the whole chain.  WINDOW_SHARES bounds it to the last 48 chain shares.
    m_required_bits = 0x1d00ffff;
    const int cap = static_cast<int>(ShareParams().WindowShares());
    BOOST_REQUIRE_EQUAL(cap, 48);
    const auto round = [&](const WitnessUnknown& miner, int i) {
        const uint32_t t = static_cast<uint32_t>(m_now + 10 * i);
        const Mined f = Mine(miner, Kind::FRUIT, 100, t, m_required_bits);
        BOOST_REQUIRE_MESSAGE(Fruit(f, reason), reason);
        const Mined c = Mine(miner, Kind::CHAIN, 100, t + 5, m_required_bits);
        BOOST_REQUIRE_MESSAGE(Chain(c, reason), reason);
    };
    int i = 0;
    for (; i < 20; ++i) round(MINER_A, i);
    for (; i < 30; ++i) round(MINER_B, i);
    // Both are inside the window.
    auto st = m_man->GetTemplateState(MINER_B, COIN, m_required_bits);
    BOOST_CHECK_EQUAL(st.payouts.size(), 2U);
    // A's last chain share (height 19) is listed by the chain share at height 20: it
    // stays in the window while that one is among the last `cap` chain shares ...
    for (; i < 20 + cap; ++i) round(MINER_B, i);
    BOOST_CHECK_EQUAL(m_man->ActiveHeight(), 20 + cap - 1);
    st = m_man->GetTemplateState(MINER_B, COIN, m_required_bits);
    BOOST_CHECK_EQUAL(st.payouts.size(), 2U);
    // ... and drops out with the next one, although the work window is nowhere near full.
    round(MINER_B, i);
    st = m_man->GetTemplateState(MINER_B, COIN, m_required_bits);
    BOOST_REQUIRE_EQUAL(st.payouts.size(), 1U);
    BOOST_CHECK(st.payouts[0].first == MINER_B);
    BOOST_CHECK_EQUAL(st.payouts[0].second, COIN);
    // A share paying A would now be rejected.
    const Mined m = Mine(MINER_B, Kind::FRUIT, 100, static_cast<uint32_t>(m_now + 10 * (i + 1)), m_required_bits, [](CMutableTransaction& cb) { cb.vout.emplace_back(1, GetScriptForDestination(MINER_A)); });
    BOOST_CHECK(!Fruit(m, reason));
    BOOST_CHECK_EQUAL(reason, "share-bad-payout");
}

BOOST_AUTO_TEST_CASE(sharechain_lwma_retarget)
{
    // Main share parameters retarget; build synthetic chains of 61 chain shares and
    // check the direction and the clamps of RequiredShareBits.
    SelectShareParams(ShareChainType::MAIN);
    ShareChainstateManager man(ShareChainstateManager::Options{
        .chainparams = Params(),
        .sharechainparams = ShareParams(),
        .adjusted_time_callback = []() { return NodeClock::now(); },
    });
    const auto build = [](uint32_t bits, int64_t spacing) {
        std::shared_ptr<ShareIndex> prev;
        for (int i = 0; i < 61; ++i) {
            auto idx = std::make_shared<ShareIndex>();
            idx->share.bits = bits;
            idx->share.height = i;
            idx->share.wit.nTime = static_cast<uint32_t>(1700000000 + i * spacing);
            idx->prev_share = prev;
            prev = idx;
        }
        return prev;
    };
    const auto target = [](uint32_t bits) { arith_uint256 t; t.SetCompact(bits); return t; };
    const uint32_t hard = 0x1b034000; // harder than ShareBits (0x1b068db2)
    LOCK(::cs_main);

    // Chain shares are expected every ChainFactor * ShareInterval = 60 s on main.
    const int64_t T = ShareParams().ShareInterval() * ShareParams().ChainFactor();
    BOOST_CHECK_EQUAL(T, 60);
    // On schedule: unchanged.
    BOOST_CHECK_EQUAL(man.RequiredShareBits(build(hard, T).get()), hard);
    // Too slow: easier, but by at most 25 %.
    const uint32_t slow = man.RequiredShareBits(build(hard, 2 * T).get());
    BOOST_CHECK(target(slow) > target(hard));
    BOOST_CHECK(target(slow) <= target(hard) / 4 * 5);
    // Too fast: harder, by at most 25 %.
    const uint32_t fast = man.RequiredShareBits(build(hard, T / 2).get());
    BOOST_CHECK(target(fast) < target(hard));
    BOOST_CHECK(target(fast) >= target(hard) / 4 * 3);
    // Never easier than the chain's easiest target.
    BOOST_CHECK_EQUAL(man.RequiredShareBits(build(ShareParams().ShareBits(), 10 * T).get()), ShareParams().ShareBits());
    // Fewer than two chain shares: the initial target.
    BOOST_CHECK_EQUAL(man.RequiredShareBits(nullptr), ShareParams().ShareBits());
    SelectShareParams(ShareChainType::REGTEST);
}

BOOST_AUTO_TEST_SUITE_END()

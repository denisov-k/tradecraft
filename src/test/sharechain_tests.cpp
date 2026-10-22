// Copyright (c) 2026 The Freicoin Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sharechain.h>

#include <addresstype.h>
#include <arith_uint256.h>
#include <crypto/common.h>
#include <test/data/sharechain_pplns.json.h>
#include <test/util/json.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

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
    BOOST_REQUIRE(tests.size() == 4);

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
        const CAmount total = test["total"].getInt<int64_t>();
        const WitnessUnknown builder = MinerFromHex(test["builder"].get_str());
        const CAmount dust = test["dust"].getInt<int64_t>();
        const size_t max_payees = static_cast<size_t>(test["max_payees"].getInt<int64_t>());

        ShareCredits credits = ComputeShareCredits(index.back().get(), window, lookup);
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

BOOST_AUTO_TEST_SUITE_END()

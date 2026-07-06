// Copyright (c) 2023-present The Bitcoin Core developers
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

#include <addresstype.h>
#include <bench/bench.h>
#include <blockfilter.h>
#include <chain.h>
#include <index/base.h>
#include <index/blockfilterindex.h>
#include <interfaces/chain.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <span.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <cassert>
#include <memory>
#include <vector>

using namespace util::hex_literals;

// Very simple block filter index sync benchmark, only using coinbase outputs.
static void BlockFilterIndexSync(benchmark::Bench& bench)
{
    const auto test_setup = MakeNoLogFileContext<TestChain100Setup>();

    // Create more blocks
    int CHAIN_SIZE = 600;
<<<<<<< v29.0
    CPubKey pubkey{"02ed26169896db86ced4cbb7b3ecef9859b5952825adbeab998fb5b307e54949c9"_hex_u8};
    CScript script = GetScriptForDestination(WitnessV0KeyHash(pubkey));
=======
    CPubKey pubkey{ParseHex("02ed26169896db86ced4cbb7b3ecef9859b5952825adbeab998fb5b307e54949c9")};
    CScript script = GetScriptForDestination(WitnessV0ShortHash(/*version=*/0, pubkey));
>>>>>>> tc-28.1
    std::vector<CMutableTransaction> noTxns;
    for (int i = 1; i < CHAIN_SIZE - 100; i++) {
        test_setup->CreateAndProcessBlock(noTxns, script);
        SetMockTime(GetTime() + 1);
    }
    assert(WITH_LOCK(::cs_main, return test_setup->m_node.chainman->ActiveHeight() == CHAIN_SIZE));

    bench.minEpochIterations(5).run([&] {
        BlockFilterIndex filter_index(interfaces::MakeChain(test_setup->m_node), BlockFilterType::BASIC,
                                      /*n_cache_size=*/0, /*f_memory=*/false, /*f_wipe=*/true);
        assert(filter_index.Init());
        assert(!filter_index.BlockUntilSyncedToCurrentChain());
        filter_index.Sync();

        IndexSummary summary = filter_index.GetSummary();
        assert(summary.synced);
        assert(summary.best_block_hash == WITH_LOCK(::cs_main, return test_setup->m_node.chainman->ActiveTip()->GetBlockHash()));
    });
}

BENCHMARK(BlockFilterIndexSync, benchmark::PriorityLevel::HIGH);

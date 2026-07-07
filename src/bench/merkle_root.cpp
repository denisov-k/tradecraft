// Copyright (c) 2016-2022 The Bitcoin Core developers
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

#include <bench/bench.h>
#include <consensus/merkle.h>
#include <random.h>
#include <uint256.h>

#include <cassert>
#include <vector>

static void MerkleRoot(benchmark::Bench& bench)
{
    FastRandomContext rng{/*fDeterministic=*/true};

    std::vector<uint256> hashes{};
    hashes.resize(9001);
    for (auto& item : hashes) {
        item = rng.rand256();
    }

    constexpr uint256 expected_root{"d8d4dfd014a533bc3941b8663fa6e7f3a8707af124f713164d75b0c3179ecb08"};
    for (bool mutate : {false, true}) {
        bench.name(mutate ? "MerkleRootWithMutation" : "MerkleRoot").batch(hashes.size()).unit("leaf").run([&] {
            std::vector<uint256> leaves;
            leaves.reserve((hashes.size() + 1) & ~1ULL); // capacity rounded up to even
            for (const auto& hash : hashes) {
                leaves.push_back(hash);
            }

            bool mutated{false};
            const uint256 root{ComputeMerkleRoot(std::move(leaves), mutate ? &mutated : nullptr)};
            assert(root == expected_root);
        });
    }
}

BENCHMARK(MerkleRoot);

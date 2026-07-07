// Copyright (c) 2023 The Bitcoin Core developers
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

#include <test/util/random.h>

#include <logging.h>
#include <random.h>
#include <uint256.h>
#include <util/check.h>

#include <cstdlib>
#include <iostream>

std::atomic<bool> g_seeded_g_prng_zero{false};

extern void MakeRandDeterministicDANGEROUS(const uint256& seed) noexcept;

void SeedRandomStateForTest(SeedRand seedtype)
{
    constexpr auto RANDOM_CTX_SEED{"RANDOM_CTX_SEED"};

    // Do this once, on the first call, regardless of seedtype, because once
    // MakeRandDeterministicDANGEROUS is called, the output of GetRandHash is
    // no longer truly random. It should be enough to get the seed once for the
    // process.
    static const auto g_ctx_seed = []() -> std::optional<uint256> {
        if constexpr (G_FUZZING) return {};
        // If RANDOM_CTX_SEED is set, use that as seed.
        if (const char* num{std::getenv(RANDOM_CTX_SEED)}) {
            if (auto num_parsed{uint256::FromUserHex(num)}) {
                return *num_parsed;
            } else {
                std::cerr << RANDOM_CTX_SEED << " must consist of up to " << uint256::size() * 2 << " hex digits (\"0x\" prefix allowed), it was set to: '" << num << "'.\n";
                std::abort();
            }
        }
        // Otherwise use a (truly) random value.
        return GetRandHash();
    }();

    g_seeded_g_prng_zero = seedtype == SeedRand::ZEROS;
    if constexpr (G_FUZZING) {
        Assert(g_seeded_g_prng_zero); // Only SeedRandomStateForTest(SeedRand::ZEROS) is allowed in fuzz tests
        Assert(!g_used_g_prng);       // The global PRNG must not have been used before SeedRandomStateForTest(SeedRand::ZEROS)
    }
    const uint256& seed{seedtype == SeedRand::FIXED_SEED ? g_ctx_seed.value() : uint256::ZERO};
    LogInfo("Setting random seed for current tests to %s=%s\n", RANDOM_CTX_SEED, seed.GetHex());
    MakeRandDeterministicDANGEROUS(seed);
}

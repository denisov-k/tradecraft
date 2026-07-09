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

#ifndef FREICOIN_TEST_UTIL_RANDOM_H
#define FREICOIN_TEST_UTIL_RANDOM_H

#include <consensus/amount.h>
#include <random.h>
#include <uint256.h>

#include <atomic>
#include <cstdint>

enum class SeedRand {
    /**
     * Seed with a compile time constant of zeros.
     */
    ZEROS,
    /**
     * Seed with a fixed value that never changes over the lifetime of this
     * process. The seed is read from the RANDOM_CTX_SEED environment variable
     * if set, otherwise generated randomly once, saved, and reused.
     */
    FIXED_SEED,
};

/** Seed the global RNG state for testing and log the seed value. This affects all randomness, except GetStrongRandBytes(). */
void SeedRandomStateForTest(SeedRand seed);

extern std::atomic<bool> g_seeded_g_prng_zero;
extern std::atomic<bool> g_used_g_prng;

template <RandomNumberGenerator Rng>
inline CAmount RandMoney(Rng&& rng)
{
    return CAmount{rng.randrange(MAX_MONEY + 1)};
}

#endif // FREICOIN_TEST_UTIL_RANDOM_H

// Copyright (c) The Bitcoin Core developers
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
#include <random.h>
#include <util/obfuscation.h>

#include <cstddef>
#include <vector>

static void ObfuscationBench(benchmark::Bench& bench)
{
    FastRandomContext frc{/*fDeterministic=*/true};
    auto data{frc.randbytes<std::byte>(1024)};
    const Obfuscation obfuscation{frc.randbytes<Obfuscation::KEY_SIZE>()};

    size_t offset{0};
    bench.batch(data.size()).unit("byte").run([&] {
        obfuscation(data, offset++); // mutated differently each time
        ankerl::nanobench::doNotOptimizeAway(data);
    });
}

BENCHMARK(ObfuscationBench, benchmark::PriorityLevel::HIGH);

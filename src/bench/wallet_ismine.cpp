// Copyright (c) 2022 The Bitcoin Core developers
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

<<<<<<< v29.0
#include <addresstype.h>
=======
#include <config/freicoin-config.h> // IWYU pragma: keep
>>>>>>> tc-28.1
#include <bench/bench.h>
#include <bitcoin-build-config.h> // IWYU pragma: keep
#include <key.h>
#include <key_io.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <wallet/context.h>
#include <wallet/db.h>
#include <wallet/test/util.h>
#include <wallet/types.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace wallet {
static void WalletIsMine(benchmark::Bench& bench, bool legacy_wallet, int num_combo = 0)
{
    const auto test_setup = MakeNoLogFileContext<TestingSetup>();

    WalletContext context;
    context.args = &test_setup->m_args;
    context.chain = test_setup->m_node.chain.get();

    // Setup the wallet
    // Loading the wallet will also create it
    uint64_t create_flags = 0;
    if (!legacy_wallet) {
        create_flags = WALLET_FLAG_DESCRIPTORS;
    }
    auto database = CreateMockableWalletDatabase();
    auto wallet = TestLoadWallet(std::move(database), context, create_flags);

    // For a descriptor wallet, fill with num_combo combo descriptors with random keys
    // This benchmarks a non-HD wallet migrated to descriptors
    if (!legacy_wallet && num_combo > 0) {
        LOCK(wallet->cs_wallet);
        for (int i = 0; i < num_combo; ++i) {
            CKey key;
            key.MakeNewKey(/*fCompressed=*/true);
            FlatSigningProvider keys;
            std::string error;
            std::vector<std::unique_ptr<Descriptor>> desc = Parse("combo(" + EncodeSecret(key) + ")", keys, error, /*require_checksum=*/false);
            WalletDescriptor w_desc(std::move(desc.at(0)), /*creation_time=*/0, /*range_start=*/0, /*range_end=*/0, /*next_index=*/0);
            auto spkm = wallet->AddWalletDescriptor(w_desc, keys, /*label=*/"", /*internal=*/false);
            assert(spkm);
        }
    }

    const CScript script = GetScriptForDestination(DecodeDestination(ADDRESS_FCRT1_UNSPENDABLE));

    bench.run([&] {
        LOCK(wallet->cs_wallet);
        isminetype mine = wallet->IsMine(script);
        assert(mine == ISMINE_NO);
    });

    TestUnloadWallet(std::move(wallet));
}

#ifdef USE_BDB
static void WalletIsMineLegacy(benchmark::Bench& bench) { WalletIsMine(bench, /*legacy_wallet=*/true); }
BENCHMARK(WalletIsMineLegacy, benchmark::PriorityLevel::LOW);
#endif

#ifdef USE_SQLITE
static void WalletIsMineDescriptors(benchmark::Bench& bench) { WalletIsMine(bench, /*legacy_wallet=*/false); }
static void WalletIsMineMigratedDescriptors(benchmark::Bench& bench) { WalletIsMine(bench, /*legacy_wallet=*/false, /*num_combo=*/2000); }
BENCHMARK(WalletIsMineDescriptors, benchmark::PriorityLevel::LOW);
BENCHMARK(WalletIsMineMigratedDescriptors, benchmark::PriorityLevel::LOW);
#endif
} // namespace wallet

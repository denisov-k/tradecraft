// Copyright (c) 2017-2022 The Bitcoin Core developers
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

#include <key.h>
#include <key_io.h>
#include <node/context.h>
#include <script/script.h>
#include <script/solver.h>
#include <script/signingprovider.h>
#include <test/util/setup_common.h>
#include <wallet/types.h>
#include <wallet/wallet.h>
#include <wallet/test/util.h>

#include <boost/test/unit_test.hpp>

using namespace util::hex_literals;

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(ismine_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(ismine_standard)
{
    CKey keys[2];
    CPubKey pubkeys[2];
    for (int i = 0; i < 2; i++) {
        keys[i].MakeNewKey(true);
        pubkeys[i] = keys[i].GetPubKey();
    }

    CKey uncompressedKey = GenerateRandomKey(/*compressed=*/false);
    CPubKey uncompressedPubkey = uncompressedKey.GetPubKey();
    std::unique_ptr<interfaces::Chain>& chain = m_node.chain;

    CScript scriptPubKey;
    bool result;


    // P2PK compressed - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "pk(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForRawPubKey(pubkeys[0]);
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }


    // P2PK uncompressed - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "pk(" + EncodeSecret(uncompressedKey) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForRawPubKey(uncompressedPubkey);
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }


    // P2PKH compressed - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "pkh(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForDestination(PKHash(pubkeys[0]));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }


    // P2PKH uncompressed - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "pkh(" + EncodeSecret(uncompressedKey) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForDestination(PKHash(uncompressedPubkey));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }


    // P2SH - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "sh(pkh(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        CScript redeemScript = GetScriptForDestination(PKHash(pubkeys[0]));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }


    // (P2PKH inside) P2SH inside P2SH (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "sh(sh(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }


    // (P2PK inside) P2SH inside P2WSH (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "wsh(sh(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }


    // P2WPK inside P2WSH (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "wsh(wpk(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }


    // (P2PK inside) P2WSH inside P2WSH (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "wsh(wsh(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }


    // P2WPK compressed - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "wpk(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForDestination(WitnessV0ShortHash(/*version=*/0, pubkeys[0]));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }


    // P2WPK uncompressed (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "wpk(" + EncodeSecret(uncompressedKey) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }


    // scriptPubKey multisig - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "multi(2," + EncodeSecret(uncompressedKey) + "," + EncodeSecret(keys[1]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForMultisig(2, {uncompressedPubkey, pubkeys[1]});
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }


    // P2SH multisig - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());

        std::string desc_str = "sh(multi(2," + EncodeSecret(uncompressedKey) + "," + EncodeSecret(keys[1]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        CScript redeemScript = GetScriptForMultisig(2, {uncompressedPubkey, pubkeys[1]});
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }


    // P2WSH multisig with compressed keys - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());

        std::string desc_str = "wsh(multi(2," + EncodeSecret(keys[0]) + "," + EncodeSecret(keys[1]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        CScript redeemScript = GetScriptForMultisig(2, {pubkeys[0], pubkeys[1]});
        scriptPubKey = GetScriptForDestination(WitnessV0LongHash(0 /* version */, redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }


    // P2WSH multisig with uncompressed key (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());

        std::string desc_str = "wsh(multi(2," + EncodeSecret(uncompressedKey) + "," + EncodeSecret(keys[1]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }


    // P2WSH multisig - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());

        std::string desc_str = "wsh(multi(2," + EncodeSecret(keys[0]) + "," + EncodeSecret(keys[1]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        CScript witnessScript = GetScriptForMultisig(2, {pubkeys[0], pubkeys[1]});
        CScript redeemScript = GetScriptForDestination(WitnessV0LongHash(0 /* version */, witnessScript));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(!result);

        result = spk_manager->IsMine(redeemScript);
        BOOST_CHECK(result);
    }

    // Combo - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());

        std::string desc_str = "combo(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        // Test P2PK
        result = spk_manager->IsMine(GetScriptForRawPubKey(pubkeys[0]));
        BOOST_CHECK(result);

        // Test P2PKH
        result = spk_manager->IsMine(GetScriptForDestination(PKHash(pubkeys[0])));
        BOOST_CHECK(result);

        // Test P2SH (combo descriptor does not describe P2SH)
        CScript redeemScript = GetScriptForDestination(PKHash(pubkeys[0]));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(!result);

        // Test P2WPK
        scriptPubKey = GetScriptForDestination(WitnessV0ShortHash(/*version=*/0, pubkeys[0]));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);

        // P2SH-P2WPK output
        redeemScript = GetScriptForDestination(WitnessV0ShortHash(/*version=*/0, pubkeys[0]));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(!result);
    }




}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet

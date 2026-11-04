// Copyright (c) 2026 The Freicoin Developers
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

#include <coins.h>
#include <consensus/assets.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(assets_tests, BasicTestingSetup)

namespace {
const std::vector<unsigned char> PROGRAM(32, 0x22);

/** A native witness v0 script-hash output, optionally ending in an extension push. */
CScript WitnessScript(const std::optional<std::vector<unsigned char>>& ext = std::nullopt)
{
    CScript script = CScript() << OP_0 << PROGRAM;
    if (ext) script << *ext;
    return script;
}

CTxOut AssetOut(const uint256& asset, CAmount amount, CAmount value = 0)
{
    return CTxOut(value, WitnessScript(AssetExtension({asset, amount})));
}

struct Fixture {
    CCoinsView base;
    CCoinsViewCache view{&base};
    uint32_t next{1};

    /** A coin in the view at `height`, and the outpoint that spends it. */
    COutPoint Add(const CTxOut& out, int height)
    {
        const COutPoint prevout(Txid::FromUint256(uint256{static_cast<uint8_t>(next++)}), 0);
        view.AddCoin(prevout, Coin(out, /*refheight=*/height, height, /*fCoinBaseIn=*/false), false);
        return prevout;
    }

    bool Check(const std::vector<COutPoint>& in, const std::vector<CTxOut>& out, std::string& reason, int asset_height = 100)
    {
        CMutableTransaction tx;
        for (const COutPoint& prevout : in) tx.vin.emplace_back(prevout);
        tx.vout = out;
        TxValidationState state;
        const bool ok = Consensus::CheckTxAssets(CTransaction(tx), view, asset_height, state);
        reason = state.GetRejectReason();
        return ok;
    }
};
} // namespace

BOOST_AUTO_TEST_CASE(extension_output_parsing)
{
    std::vector<unsigned char> ext;
    BOOST_CHECK(!WitnessScript().GetExtensionOutput(ext));
    BOOST_CHECK(WitnessScript(std::vector<unsigned char>{0x01, 0x02, 0x03}).GetExtensionOutput(ext));
    BOOST_CHECK(ext == (std::vector<unsigned char>{0x01, 0x02, 0x03}));
    // after a shard specifier, one byte or two
    CScript sharded = CScript() << OP_0 << PROGRAM << OP_5 << std::vector<unsigned char>{0x07, 0x08};
    BOOST_CHECK(sharded.GetExtensionOutput(ext));
    BOOST_CHECK(ext == (std::vector<unsigned char>{0x07, 0x08}));
    CScript sharded2 = CScript() << OP_0 << PROGRAM;
    sharded2.push_back(0x01);
    sharded2.push_back(0x20);
    sharded2 << std::vector<unsigned char>{0x09, 0x0a};
    BOOST_CHECK(sharded2.GetExtensionOutput(ext));
    BOOST_CHECK(ext == (std::vector<unsigned char>{0x09, 0x0a}));
    BOOST_CHECK(!(CScript() << OP_0 << PROGRAM << OP_5).GetExtensionOutput(ext));
    // not a witness program
    BOOST_CHECK(!(CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 1) << OP_EQUALVERIFY << OP_CHECKSIG).GetExtensionOutput(ext));

    const uint256 asset{0xab};
    AssetAmount a;
    BOOST_CHECK(GetAssetOutput(AssetOut(asset, 1234), a) == AssetOutputType::ASSET);
    BOOST_CHECK(a.asset == asset);
    BOOST_CHECK_EQUAL(a.amount, 1234);
    BOOST_CHECK(GetAssetOutput(CTxOut(0, WitnessScript()), a) == AssetOutputType::NONE);
    // an unknown extension version is left to future rules
    BOOST_CHECK(GetAssetOutput(CTxOut(0, WitnessScript(std::vector<unsigned char>(41, 0x02))), a) == AssetOutputType::NONE);
    std::vector<unsigned char> short_ext = AssetExtension({asset, 1});
    short_ext.pop_back();
    BOOST_CHECK(GetAssetOutput(CTxOut(0, WitnessScript(short_ext)), a) == AssetOutputType::MALFORMED);
}

BOOST_AUTO_TEST_CASE(asset_issuance)
{
    Fixture f;
    std::string reason;
    const COutPoint fund = f.Add(CTxOut(COIN, WitnessScript()), 150);
    const uint256 id = AssetIssuanceId(fund);
    // the asset of the first input's outpoint, in any amount and number of outputs
    BOOST_CHECK(f.Check({fund}, {AssetOut(id, 1000), AssetOut(id, MAX_MONEY - 1000)}, reason));
    // any other asset is not this transaction's to issue
    BOOST_CHECK(!f.Check({fund}, {AssetOut(uint256{0x01}, 1)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-asset-in-belowout");
    // only the first input issues
    const COutPoint fund2 = f.Add(CTxOut(COIN, WitnessScript()), 150);
    BOOST_CHECK(!f.Check({fund, fund2}, {AssetOut(AssetIssuanceId(fund2), 1)}, reason));
    BOOST_CHECK(f.Check({fund2, fund}, {AssetOut(AssetIssuanceId(fund2), 1)}, reason));
    // the ids differ by outpoint
    BOOST_CHECK(AssetIssuanceId(fund) != AssetIssuanceId(fund2));
}

BOOST_AUTO_TEST_CASE(asset_transfers)
{
    Fixture f;
    std::string reason;
    const uint256 x{0x0a}, y{0x0b};
    const COutPoint fee = f.Add(CTxOut(COIN, WitnessScript()), 150);
    const COutPoint hold_x = f.Add(AssetOut(x, 1000), 150);
    const COutPoint hold_y = f.Add(AssetOut(y, 7), 160);

    BOOST_CHECK(f.Check({fee, hold_x}, {AssetOut(x, 600), AssetOut(x, 400)}, reason));
    BOOST_CHECK(!f.Check({fee, hold_x}, {AssetOut(x, 600), AssetOut(x, 401)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-asset-in-belowout");
    // the remainder may be burned
    BOOST_CHECK(f.Check({fee, hold_x}, {AssetOut(x, 600)}, reason));
    BOOST_CHECK(f.Check({fee, hold_x}, {CTxOut(COIN / 2, WitnessScript())}, reason));
    // one transaction may move several assets: a swap
    BOOST_CHECK(f.Check({fee, hold_x, hold_y}, {AssetOut(y, 7), AssetOut(x, 1000), CTxOut(COIN / 2, WitnessScript())}, reason));
    BOOST_CHECK(!f.Check({fee, hold_x}, {AssetOut(y, 1)}, reason));
}

BOOST_AUTO_TEST_CASE(asset_output_rules)
{
    Fixture f;
    std::string reason;
    const COutPoint fund = f.Add(CTxOut(COIN, WitnessScript()), 150);
    const uint256 id = AssetIssuanceId(fund);

    BOOST_CHECK(!f.Check({fund}, {AssetOut(id, 10, /*value=*/1)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-asset-nonzero-value");
    BOOST_CHECK(!f.Check({fund}, {AssetOut(id, 0)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-asset-amount-outofrange");
    BOOST_CHECK(!f.Check({fund}, {AssetOut(id, -1)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-asset-amount-outofrange");
    BOOST_CHECK(!f.Check({fund}, {AssetOut(id, MAX_MONEY + 1)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-asset-amount-outofrange");
    BOOST_CHECK(!f.Check({fund}, {AssetOut(id, MAX_MONEY), AssetOut(id, 1)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-asset-outputs-outofrange");
    std::vector<unsigned char> long_ext = AssetExtension({id, 1});
    long_ext.push_back(0);
    BOOST_CHECK(!f.Check({fund}, {CTxOut(0, WitnessScript(long_ext))}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-asset-malformed");

    // a coinbase issues nothing
    CMutableTransaction cb;
    cb.vin.emplace_back(COutPoint());
    cb.vin[0].scriptSig = CScript() << 1 << OP_0;
    cb.vout.push_back(AssetOut(AssetIssuanceId(COutPoint()), 1));
    TxValidationState state;
    BOOST_CHECK(!Consensus::CheckTxAssets(CTransaction(cb), f.view, 100, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-asset-coinbase");
}

BOOST_AUTO_TEST_CASE(asset_claims_before_activation)
{
    Fixture f;
    std::string reason;
    const uint256 x{0x0a};
    const COutPoint fee = f.Add(CTxOut(COIN, WitnessScript()), 150);
    // made at height 99, before the rules activated at 100: nobody checked it
    const COutPoint forged = f.Add(AssetOut(x, 1000), 99);
    BOOST_CHECK(!f.Check({fee, forged}, {AssetOut(x, 1000)}, reason, /*asset_height=*/100));
    BOOST_CHECK_EQUAL(reason, "bad-txns-asset-in-belowout");
    BOOST_CHECK(f.Check({fee, forged}, {AssetOut(x, 1000)}, reason, /*asset_height=*/99));
    // and spending it with no asset output is fine: it is a zero-value output
    BOOST_CHECK(f.Check({fee, forged}, {CTxOut(COIN / 2, WitnessScript())}, reason, /*asset_height=*/100));
}

BOOST_AUTO_TEST_SUITE_END()

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

BOOST_AUTO_TEST_CASE(scrip_ledger_transactions)
{
    Fixture f;
    std::string reason;
    const uint256 scrip = ScripAssetId();
    const CTxOut pegin(10 * COIN, LedgerBase() << LedgerExtension(EXTENSION_PEGIN, 0));
    CAmount amount = -1;
    BOOST_CHECK(GetLedgerOutput(pegin, amount) == LedgerOutputType::PEGIN);
    // a peg-in on any other program would not reach the pool
    BOOST_CHECK(GetLedgerOutput(CTxOut(COIN, WitnessScript(LedgerExtension(EXTENSION_PEGIN, 0))), amount) == LedgerOutputType::MALFORMED);

    // A peg-in mints at most its scrip value (the fixture's transactions have lock_height 0).
    const COutPoint fund = f.Add(CTxOut(20 * COIN, WitnessScript()), 150);
    const CAmount minted = FreicoinToScrip(10 * COIN, 0);
    BOOST_CHECK(f.Check({fund}, {pegin, AssetOut(scrip, minted)}, reason));
    BOOST_CHECK(!f.Check({fund}, {pegin, AssetOut(scrip, minted + 1)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-asset-in-belowout");
    // nobody issues scrip but the ledger
    BOOST_CHECK(!f.Check({fund}, {AssetOut(scrip, 1)}, reason));

    // A peg-out consumes scrip like an asset output, and holds no freicoin.
    const COutPoint held = f.Add(AssetOut(scrip, 1000), 150);
    const CTxOut pegout(0, WitnessScript(LedgerExtension(EXTENSION_PEGOUT, 600)));
    BOOST_CHECK(GetLedgerOutput(pegout, amount) == LedgerOutputType::PEGOUT);
    BOOST_CHECK_EQUAL(amount, 600);
    BOOST_CHECK(f.Check({fund, held}, {pegout, AssetOut(scrip, 400)}, reason));
    BOOST_CHECK(!f.Check({fund, held}, {pegout, AssetOut(scrip, 401)}, reason));
    BOOST_CHECK(!f.Check({fund, held}, {CTxOut(1, pegout.scriptPubKey)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-pegout-nonzero-value");

    // Peg-outs are spent for good; the pool and peg-ins only by the block-final transaction.
    const COutPoint paid = f.Add(pegout, 150);
    BOOST_CHECK(!f.Check({fund, paid}, {}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-pegout-spend");
    const COutPoint claimed = f.Add(pegin, 150);
    BOOST_CHECK(!f.Check({fund, claimed}, {}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-txns-ledger-spend");
    CMutableTransaction final_tx;
    final_tx.vin.emplace_back(claimed);
    final_tx.vout.emplace_back(10 * COIN, LedgerBase() << LedgerExtension(EXTENSION_POOL, minted));
    TxValidationState state;
    BOOST_CHECK(Consensus::CheckTxAssets(CTransaction(final_tx), f.view, 100, state, /*block_final=*/true));
    BOOST_CHECK(!Consensus::CheckTxAssets(CTransaction(final_tx), f.view, 100, state, /*block_final=*/false));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-ledger-pool");
}

BOOST_AUTO_TEST_CASE(scrip_ledger_settlement)
{
    const int height = 1000;
    BlockLedger ledger;
    ledger.prior_value = 5 * COIN;
    ledger.prior_scrip = FreicoinToScrip(5 * COIN, height);
    const CScript dest = WitnessScript();
    // two peg-outs: the first paid in full, the second capped by what is left
    ledger.pegouts.emplace_back(dest, ledger.prior_scrip / 2);
    ledger.pegouts.emplace_back(dest, ledger.prior_scrip);
    const LedgerResult r = ComputeLedger(ledger, height);
    BOOST_REQUIRE_EQUAL(r.payouts.size(), 2U);
    BOOST_CHECK_EQUAL(r.payouts[0].second, ScripToFreicoin(ledger.prior_scrip / 2, height));
    BOOST_CHECK_EQUAL(r.payouts[0].second + r.payouts[1].second, 5 * COIN);
    BOOST_CHECK_EQUAL(r.pool_value, 0);
    BOOST_CHECK_EQUAL(r.pool_scrip, ledger.prior_scrip - ledger.prior_scrip / 2 - ledger.prior_scrip);
    // a peg-in adds its present value and its scrip
    BlockLedger in;
    CMutableTransaction tx;
    tx.lock_height = height - 10;
    tx.vout.emplace_back(COIN, LedgerBase() << LedgerExtension(EXTENSION_PEGIN, 0));
    in.AddTransaction(CTransaction(tx), height);
    BOOST_CHECK_EQUAL(in.pegins.size(), 1U);
    BOOST_CHECK_EQUAL(in.pegin_value, GetTimeAdjustedValue(COIN, 10));
    BOOST_CHECK_EQUAL(in.minted, FreicoinToScrip(COIN, height - 10));
    const LedgerResult r2 = ComputeLedger(in, height);
    BOOST_CHECK_EQUAL(r2.pool_value, in.pegin_value);
    BOOST_CHECK_EQUAL(r2.pool_scrip, in.minted);
    // the pool covers its scrip: the peg-in's rounding stays with it
    BOOST_CHECK_GE(r2.pool_value + 1, ScripToFreicoin(r2.pool_scrip, height));
}

BOOST_AUTO_TEST_SUITE_END()

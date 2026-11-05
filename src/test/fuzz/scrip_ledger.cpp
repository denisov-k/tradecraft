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

#include <consensus/amount.h>
#include <consensus/assets.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>

#include <algorithm>
#include <cassert>
#include <vector>

FUZZ_TARGET(scrip_ledger)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    const int height = fdp.ConsumeIntegralInRange<int>(1, 5039999);

    // exact payments in scrip
    const CAmount scrip = fdp.ConsumeIntegralInRange<CAmount>(1, 10000 * COIN);
    const CAmount value = FreicoinForScrip(scrip, height);
    if (MoneyRange(value)) {
        assert(FreicoinToScrip(value, height) == scrip);
        assert(FreicoinToScrip(value - 1, height) < scrip);
    }

    // a ledger the transaction rules allow
    BlockLedger ledger;
    ledger.prior_scrip = fdp.ConsumeIntegralInRange<CAmount>(0, 10000 * COIN);
    ledger.prior_value = ScripToFreicoin(ledger.prior_scrip, height) + fdp.ConsumeIntegralInRange<CAmount>(0, 1000);
    const CScript base = CScript() << OP_0 << std::vector<unsigned char>(32, 0x22);
    LIMITED_WHILE(fdp.ConsumeBool(), 8) {
        CMutableTransaction tx;
        tx.lock_height = height - fdp.ConsumeIntegralInRange<int>(0, std::min(height, 1000));
        tx.vout.emplace_back(fdp.ConsumeIntegralInRange<CAmount>(1, 1000 * COIN), LedgerBase() << LedgerExtension(EXTENSION_PEGIN, 0));
        ledger.AddTransaction(CTransaction(tx), height);
    }
    // the peg-outs due were made D blocks ago: out of the scrip the prior pool backs
    CAmount payable = ledger.prior_scrip;
    CAmount outstanding = ledger.prior_scrip + ledger.minted;
    LIMITED_WHILE(fdp.ConsumeBool() && payable > 0, 8) {
        const CAmount out = fdp.ConsumeIntegralInRange<CAmount>(1, payable);
        ledger.pegouts.emplace_back(base, out);
        payable -= out;
        outstanding -= out;
    }
    // fees in scrip, out of what holders still have (the minted scrip included)
    CAmount feeable = payable + ledger.minted;
    size_t fees = 0;
    LIMITED_WHILE(fdp.ConsumeBool() && feeable > 0, 8) {
        const CAmount fee = fdp.ConsumeIntegralInRange<CAmount>(1, std::min<CAmount>(feeable, MAX_MONEY));
        CMutableTransaction tx;
        tx.vout.emplace_back(0, LedgerBase() << LedgerExtension(EXTENSION_FEE, fee));
        ledger.AddTransaction(CTransaction(tx), height);
        feeable -= fee;
        outstanding -= fee;
        ++fees;
    }
    const CAmount slack = static_cast<CAmount>(ledger.pegins.size() + ledger.pegouts.size() + fees) + 1;

    const LedgerResult r = ComputeLedger(ledger, height);
    assert(r.payouts.size() == ledger.pegouts.size());
    CAmount paid = 0;
    for (size_t i = 0; i < r.payouts.size(); ++i) {
        const CAmount full = ScripToFreicoin(ledger.pegouts[i].second, height);
        assert(r.payouts[i].second >= 0 && r.payouts[i].second <= full);
        assert(r.payouts[i].second + slack >= full);
        paid += r.payouts[i].second;
    }
    // the fees reach the coinbase at their freicoin value, short of a kria a fee at most
    const CAmount full_fee = ScripToFreicoin(ledger.fee_scrip, height);
    assert(r.fee_value >= 0 && r.fee_value <= full_fee);
    assert(r.fee_value + slack >= full_fee);
    assert(paid + r.fee_value + r.pool_value == ledger.prior_value + ledger.pegin_value);
    assert(r.pool_value >= 0);
    assert(r.pool_scrip == outstanding);
    assert(r.pool_value + slack >= ScripToFreicoin(r.pool_scrip, height));
}

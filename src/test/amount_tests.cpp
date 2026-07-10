// Copyright (c) 2016-2021 The Bitcoin Core developers
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

#include <consensus/amount.h>
#include <policy/feerate.h>

#include <limits>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(amount_tests)

BOOST_AUTO_TEST_CASE(MoneyRangeTest)
{
    BOOST_CHECK_EQUAL(MoneyRange(CAmount(-1)), false);
    BOOST_CHECK_EQUAL(MoneyRange(CAmount(0)), true);
    BOOST_CHECK_EQUAL(MoneyRange(CAmount(1)), true);
    BOOST_CHECK_EQUAL(MoneyRange(MAX_MONEY), true);
    BOOST_CHECK_EQUAL(MoneyRange(MAX_MONEY + CAmount(1)), false);
}

BOOST_AUTO_TEST_CASE(GetFeeTest)
{
    CFeeRate feeRate, altFeeRate;

    feeRate = CFeeRate(0);
    // Must always return 0
    BOOST_CHECK_EQUAL(feeRate.GetFee(0), CAmount(0));
    BOOST_CHECK_EQUAL(feeRate.GetFee(1e5), CAmount(0));

    feeRate = CFeeRate(1000);
    // Must always just return the arg
    BOOST_CHECK_EQUAL(feeRate.GetFee(0), CAmount(0));
    BOOST_CHECK_EQUAL(feeRate.GetFee(1), CAmount(1));
    BOOST_CHECK_EQUAL(feeRate.GetFee(121), CAmount(121));
    BOOST_CHECK_EQUAL(feeRate.GetFee(999), CAmount(999));
    BOOST_CHECK_EQUAL(feeRate.GetFee(1e3), CAmount(1e3));
    BOOST_CHECK_EQUAL(feeRate.GetFee(9e3), CAmount(9e3));

    feeRate = CFeeRate(-1000);
    // Must always just return -1 * arg
    BOOST_CHECK_EQUAL(feeRate.GetFee(0), CAmount(0));
    BOOST_CHECK_EQUAL(feeRate.GetFee(1), CAmount(-1));
    BOOST_CHECK_EQUAL(feeRate.GetFee(121), CAmount(-121));
    BOOST_CHECK_EQUAL(feeRate.GetFee(999), CAmount(-999));
    BOOST_CHECK_EQUAL(feeRate.GetFee(1e3), CAmount(-1e3));
    BOOST_CHECK_EQUAL(feeRate.GetFee(9e3), CAmount(-9e3));

    feeRate = CFeeRate(123);
    // Rounds up the result, if not integer
    BOOST_CHECK_EQUAL(feeRate.GetFee(0), CAmount(0));
    BOOST_CHECK_EQUAL(feeRate.GetFee(8), CAmount(1)); // Special case: returns 1 instead of 0
    BOOST_CHECK_EQUAL(feeRate.GetFee(9), CAmount(2));
    BOOST_CHECK_EQUAL(feeRate.GetFee(121), CAmount(15));
    BOOST_CHECK_EQUAL(feeRate.GetFee(122), CAmount(16));
    BOOST_CHECK_EQUAL(feeRate.GetFee(999), CAmount(123));
    BOOST_CHECK_EQUAL(feeRate.GetFee(1e3), CAmount(123));
    BOOST_CHECK_EQUAL(feeRate.GetFee(9e3), CAmount(1107));

    feeRate = CFeeRate(-123);
    // Truncates the result, if not integer
    BOOST_CHECK_EQUAL(feeRate.GetFee(0), CAmount(0));
    BOOST_CHECK_EQUAL(feeRate.GetFee(8), CAmount(-1)); // Special case: returns -1 instead of 0
    BOOST_CHECK_EQUAL(feeRate.GetFee(9), CAmount(-1));

    // check alternate constructor
    feeRate = CFeeRate(1000);
    altFeeRate = CFeeRate(feeRate);
    BOOST_CHECK_EQUAL(feeRate.GetFee(100), altFeeRate.GetFee(100));

    // Check full constructor
    BOOST_CHECK(CFeeRate(CAmount(-1), 0) == CFeeRate(0));
    BOOST_CHECK(CFeeRate(CAmount(0), 0) == CFeeRate(0));
    BOOST_CHECK(CFeeRate(CAmount(1), 0) == CFeeRate(0));
    BOOST_CHECK(CFeeRate(CAmount(1), -1000) == CFeeRate(0));
    // default value
    BOOST_CHECK(CFeeRate(CAmount(-1), 1000) == CFeeRate(-1));
    BOOST_CHECK(CFeeRate(CAmount(0), 1000) == CFeeRate(0));
    BOOST_CHECK(CFeeRate(CAmount(1), 1000) == CFeeRate(1));
    // Previously, precision was limited to three decimal digits
    // due to only supporting kria per kB, so CFeeRate(CAmount(1), 1001) was equal to CFeeRate(0)
    // Since #32750, higher precision is maintained.
    BOOST_CHECK(CFeeRate(CAmount(1), 1001) > CFeeRate(0) && CFeeRate(CAmount(1), 1001) < CFeeRate(1));
    BOOST_CHECK(CFeeRate(CAmount(2), 1001) > CFeeRate(1) && CFeeRate(CAmount(2), 1001) < CFeeRate(2));
    // some more integer checks
    BOOST_CHECK(CFeeRate(CAmount(26), 789) > CFeeRate(32) && CFeeRate(CAmount(26), 789) < CFeeRate(33));
    BOOST_CHECK(CFeeRate(CAmount(27), 789) > CFeeRate(34) && CFeeRate(CAmount(27), 789) < CFeeRate(35));
    // Maximum size in bytes, should not crash
    CFeeRate(MAX_MONEY, std::numeric_limits<int32_t>::max()).GetFeePerK();

    // check multiplication operator
    // check multiplying by zero
    feeRate = CFeeRate(1000);
    BOOST_CHECK(0 * feeRate == CFeeRate(0));
    BOOST_CHECK(feeRate * 0 == CFeeRate(0));
    // check multiplying by a positive integer
    BOOST_CHECK(3 * feeRate == CFeeRate(3000));
    BOOST_CHECK(feeRate * 3 == CFeeRate(3000));
    // check multiplying by a negative integer
    BOOST_CHECK(-3 * feeRate == CFeeRate(-3000));
    BOOST_CHECK(feeRate * -3 == CFeeRate(-3000));
    // check commutativity
    BOOST_CHECK(2 * feeRate == feeRate * 2);
    // check with large numbers
    int largeNumber = 1000000;
    BOOST_CHECK(largeNumber * feeRate == feeRate * largeNumber);
    // check boundary values
    int maxInt = std::numeric_limits<int>::max();
    feeRate = CFeeRate(maxInt);
    BOOST_CHECK(feeRate * 2 == CFeeRate(static_cast<int64_t>(maxInt) * 2));
    BOOST_CHECK(2 * feeRate == CFeeRate(static_cast<int64_t>(maxInt) * 2));
    // check with zero fee rate
    feeRate = CFeeRate(0);
    BOOST_CHECK(feeRate * 5 == CFeeRate(0));
    BOOST_CHECK(5 * feeRate == CFeeRate(0));
}

BOOST_AUTO_TEST_CASE(BinaryOperatorTest)
{
    CFeeRate a, b;
    a = CFeeRate(1);
    b = CFeeRate(2);
    BOOST_CHECK(a < b);
    BOOST_CHECK(b > a);
    BOOST_CHECK(a == a);
    BOOST_CHECK(a <= b);
    BOOST_CHECK(a <= a);
    BOOST_CHECK(b >= a);
    BOOST_CHECK(b >= b);
    // a should be 0.00000002 FRC/kvB now
    a += a;
    BOOST_CHECK(a == b);
}

BOOST_AUTO_TEST_CASE(ToStringTest)
{
    CFeeRate feeRate;
    feeRate = CFeeRate(1);
    BOOST_CHECK_EQUAL(feeRate.ToString(), "0.00000001 FRC/kvB");
    BOOST_CHECK_EQUAL(feeRate.ToString(FeeRateFormat::FRC_KVB), "0.00000001 FRC/kvB");
    BOOST_CHECK_EQUAL(feeRate.ToString(FeeRateFormat::SAT_VB), "0.001 sat/vB");
}

BOOST_AUTO_TEST_CASE(TimeAdjustValueForwardK_test)
{
    // nVersion=3-lite per-asset demurrage: at k=20 it must be bit-identical to the host
    // currency, and it must match the reference model's golden vectors for other rates.
    // Golden values from research/nversion3/verify_kernel.py (96-guard-bit ladder).
    for (uint32_t d : {1u, 2u, 96u, 1000u, 52560u, 485000u}) {
        BOOST_CHECK_EQUAL(TimeAdjustValueForwardK(1234567890LL, d, 20),
                          TimeAdjustValueForward(1234567890LL, d));
    }
    // k=18 (faster demurrage) and k=22 (slower), value 100000000 (1 FRC nominal):
    BOOST_CHECK_EQUAL(TimeAdjustValueForwardK(100000000LL, 1,      18), 99999618LL);
    BOOST_CHECK_EQUAL(TimeAdjustValueForwardK(100000000LL, 96,     18), 99963385LL);
    BOOST_CHECK_EQUAL(TimeAdjustValueForwardK(100000000LL, 1000,   18), 99619256LL);
    // faster rate melts more than the host currency at the same distance
    BOOST_CHECK(TimeAdjustValueForwardK(100000000LL, 1000, 18) <
                TimeAdjustValueForward(100000000LL, 1000));
    // slower rate melts less
    BOOST_CHECK(TimeAdjustValueForwardK(100000000LL, 1000, 22) >
                TimeAdjustValueForward(100000000LL, 1000));
    // distance 0 and the decay-to-zero cutoff behave like the host kernel
    BOOST_CHECK_EQUAL(TimeAdjustValueForwardK(100000000LL, 0, 18), 100000000LL);
    BOOST_CHECK_EQUAL(TimeAdjustValueForwardK(100000000LL, (uint32_t)1 << 26, 18), 0LL);
}

BOOST_AUTO_TEST_SUITE_END()

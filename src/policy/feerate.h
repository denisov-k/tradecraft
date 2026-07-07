// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
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

#ifndef FREICOIN_POLICY_FEERATE_H
#define FREICOIN_POLICY_FEERATE_H

#include <consensus/amount.h>
#include <serialize.h>
#include <util/feefrac.h>


#include <cstdint>
#include <string>
#include <type_traits>

const std::string CURRENCY_UNIT = "FRC"; // One formatted unit
const std::string CURRENCY_ATOM = "sat"; // One indivisible minimum value unit

/* Used to determine type of fee estimation requested */
enum class FeeEstimateMode {
    UNSET,        //!< Use default settings based on other criteria
    ECONOMICAL,   //!< Force estimateSmartFee to use non-conservative estimates
    CONSERVATIVE, //!< Force estimateSmartFee to use conservative estimates
    FRC_KVB,      //!< Use FRC/kvB fee rate unit
    SAT_VB,       //!< Use sat/vB fee rate unit
};

/**
 * Fee rate in satoshis per virtualbyte: CAmount / vB
 * the feerate is represented internally as FeeFrac
 */
class CFeeRate
{
private:
    /** Fee rate in sats/vB (satoshis per N virtualbytes) */
    FeePerVSize m_feerate;

public:
    /** Fee rate of 0 satoshis per 0 vB */
    CFeeRate() = default;
    template<std::integral I> // Disallow silent float -> int conversion
    explicit CFeeRate(const I m_feerate_kvb) : m_feerate(FeePerVSize(m_feerate_kvb, 1000)) {}

    /**
     * Construct a fee rate from a fee in kria and a vsize in vB.
     *
     * Passing any virtual_bytes less than or equal to 0 will result in 0 fee rate per 0 size.
     */
    CFeeRate(const CAmount& nFeePaid, int32_t virtual_bytes);

    /**
     * Return the fee in kria for the given vsize in vbytes.
     * If the calculated fee would have fractional kria, then the
     * returned fee will always be rounded up to the nearest kria.
     */
    CAmount GetFee(int32_t virtual_bytes) const;

    /**
     * Return the fee in kria for a vsize of 1000 vbytes
     */
    CAmount GetFeePerK() const { return CAmount(m_feerate.EvaluateFeeDown(1000)); }
    friend std::weak_ordering operator<=>(const CFeeRate& a, const CFeeRate& b) noexcept
    {
        return FeeRateCompare(a.m_feerate, b.m_feerate);
    }
    friend bool operator==(const CFeeRate& a, const CFeeRate& b) noexcept
    {
        return FeeRateCompare(a.m_feerate, b.m_feerate) == std::weak_ordering::equivalent;
    }
    CFeeRate& operator+=(const CFeeRate& a) {
        m_feerate = FeePerVSize(GetFeePerK() + a.GetFeePerK(), 1000);
        return *this;
    }
    std::string ToString(const FeeEstimateMode& fee_estimate_mode = FeeEstimateMode::BTC_KVB) const;
    friend CFeeRate operator*(const CFeeRate& f, int a) { return CFeeRate(a * f.m_feerate.fee, f.m_feerate.size); }
    friend CFeeRate operator*(int a, const CFeeRate& f) { return CFeeRate(a * f.m_feerate.fee, f.m_feerate.size); }

    SERIALIZE_METHODS(CFeeRate, obj) { READWRITE(obj.m_feerate.fee, obj.m_feerate.size); }
};

#endif // FREICOIN_POLICY_FEERATE_H

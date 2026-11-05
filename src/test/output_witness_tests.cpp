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

#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

namespace {
CMutableTransaction SampleTx()
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint(Txid::FromUint256(uint256{1}), 0));
    tx.vin[0].scriptWitness.stack = {{0x01, 0x02}};
    tx.vout.emplace_back(0, CScript() << OP_0 << std::vector<unsigned char>(32, 0x22));
    tx.vout.emplace_back(0, CScript() << OP_0 << std::vector<unsigned char>(32, 0x33));
    tx.lock_height = 7;
    return tx;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(output_witness_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(serialization)
{
    const CMutableTransaction plain = SampleTx();
    CMutableTransaction with = plain;
    with.output_witness.resize(2);
    with.output_witness[1].stack = {std::vector<unsigned char>(100, 0xab)};
    const CTransaction a(plain), b(with);

    // Neither the txid nor the wtxid sees them: nodes without the rules agree on both.
    BOOST_CHECK(b.HasOutputWitness());
    BOOST_CHECK(!a.HasOutputWitness());
    BOOST_CHECK(a.GetHash() == b.GetHash());
    BOOST_CHECK(a.GetWitnessHash() == b.GetWitnessHash());
    BOOST_CHECK(a.GetOutputWitnessHash().IsNull());
    BOOST_CHECK(!b.GetOutputWitnessHash().IsNull());

    // Flag 2 carries them; without it (input witnesses only) the transaction is the old one.
    DataStream full, input_only;
    full << TX_WITH_WITNESS(b);
    input_only << TX_WITH_INPUT_WITNESS(b);
    DataStream old;
    old << TX_WITH_WITNESS(a);
    BOOST_CHECK(input_only.str() == old.str());
    BOOST_CHECK_EQUAL(full.size(), old.size() + 1 + 1 + 1 + 100); // two stacks: empty, then one item of 100 bytes
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(full.str()[5]), 3); // flags: input and output witnesses

    CMutableTransaction back;
    full >> TX_WITH_WITNESS(back);
    BOOST_CHECK(CTransaction(back).GetOutputWitnessHash() == b.GetOutputWitnessHash());
    BOOST_REQUIRE_EQUAL(back.output_witness.size(), 2U);
    BOOST_CHECK(back.output_witness[0].IsNull());

    // A reader that does not take them (as old nodes) refuses the flag.
    DataStream again;
    again << TX_WITH_WITNESS(b);
    CMutableTransaction refused;
    BOOST_CHECK_THROW(again >> TX_WITH_INPUT_WITNESS(refused), std::ios_base::failure);

    // An all-empty output witness record is not allowed.
    CMutableTransaction empty = plain;
    empty.output_witness.resize(2);
    DataStream e;
    e << TX_WITH_WITNESS(CTransaction(empty)); // nothing to write: no flag
    BOOST_CHECK(e.str() == old.str());

    // They weigh as witness data.
    BOOST_CHECK_EQUAL(GetTransactionWeight(b), GetTransactionWeight(a) + 1 + 1 + 1 + 100);
}

BOOST_AUTO_TEST_SUITE_END()

struct ConfidentialChainSetup : public TestChain100Setup {
    ConfidentialChainSetup() : TestChain100Setup(ChainType::REGTEST, {.extra_args = {"-vbparams=confidential:-1:9223372036854775807"}}) {}
};

BOOST_FIXTURE_TEST_SUITE(output_witness_block_tests, ConfidentialChainSetup)

BOOST_AUTO_TEST_CASE(block_commitment)
{
    const CScript script = CScript() << OP_TRUE;
    // A block from the miner commits to its (empty) output witness tree and is accepted.
    const CBlock block = CreateAndProcessBlock({}, script);
    BOOST_CHECK(m_node.chainman->ActiveChain().Tip()->GetBlockHash() == block.GetHash());
    BOOST_REQUIRE(!block.vtx[0]->vin[0].scriptWitness.stack.empty());
    const auto& branch = block.vtx[0]->vin[0].scriptWitness.stack[0];
    BOOST_REQUIRE_EQUAL(branch.size(), 32U);
    BOOST_CHECK(uint256(branch) == BlockOutputWitnessMerkleRoot(block));
    const Consensus::Params& params = m_node.chainman->GetConsensus();
    BOOST_CHECK(!IsBlockMutated(block, params, /*check_witness_root=*/true, /*check_output_witness_root=*/true));

    // Output witnesses added to (or stripped from) a transaction change the output
    // witness root but neither the txid nor the wtxid: the block reads as mutated.
    CBlock changed = CreateBlock({}, script, m_node.chainman->ActiveChainstate());
    BOOST_CHECK(!IsBlockMutated(changed, params, true, true));
    CMutableTransaction final_tx(*changed.vtx.back());
    final_tx.output_witness.resize(final_tx.vout.size());
    final_tx.output_witness[0].stack = {{0x01}};
    changed.vtx.back() = MakeTransactionRef(std::move(final_tx));
    changed.m_checked_witness_commitment = false; // the block caches the check
    BOOST_CHECK(IsBlockMutated(changed, params, true, true));

    // The coinbase witness carries the branch: without it the block is mutated.
    CBlock bare = CreateBlock({}, script, m_node.chainman->ActiveChainstate());
    CMutableTransaction cb(*bare.vtx[0]);
    cb.vin[0].scriptWitness.stack.clear();
    bare.vtx[0] = MakeTransactionRef(std::move(cb));
    bare.m_checked_witness_commitment = false;
    BOOST_CHECK(IsBlockMutated(bare, params, true, true));
}

BOOST_AUTO_TEST_SUITE_END()

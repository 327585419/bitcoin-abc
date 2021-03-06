// Copyright (c) 2011-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <config.h>
#include <consensus/validation.h>
#include <key.h>
#include <keystore.h>
#include <miner.h>
#include <policy/policy.h>
#include <pubkey.h>
#include <random.h>
#include <script/scriptcache.h>
#include <script/sighashtype.h>
#include <script/sign.h>
#include <txmempool.h>
#include <util/time.h>
#include <validation.h>

#include <test/lcg.h>
#include <test/sigutil.h>
#include <test/test_bitcoin.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(txvalidationcache_tests)

static bool ToMemPool(const CMutableTransaction &tx) {
    LOCK(cs_main);

    CValidationState state;
    return AcceptToMemoryPool(GetConfig(), g_mempool, state,
                              MakeTransactionRef(tx), nullptr, true,
                              Amount::zero());
}

BOOST_FIXTURE_TEST_CASE(tx_mempool_block_doublespend, TestChain100Setup) {
    // Make sure skipping validation of transactions that were validated going
    // into the memory pool does not allow double-spends in blocks to pass
    // validation when they should not.
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey())
                                     << OP_CHECKSIG;

    // Create a double-spend of mature coinbase txn:
    std::vector<CMutableTransaction> spends;
    spends.resize(2);
    for (int i = 0; i < 2; i++) {
        spends[i].nVersion = 1;
        spends[i].vin.resize(1);
        spends[i].vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetId(), 0);
        spends[i].vout.resize(1);
        spends[i].vout[0].nValue = 11 * CENT;
        spends[i].vout[0].scriptPubKey = scriptPubKey;

        // Sign:
        std::vector<uint8_t> vchSig;
        uint256 hash = SignatureHash(scriptPubKey, CTransaction(spends[i]), 0,
                                     SigHashType().withForkId(),
                                     m_coinbase_txns[0]->vout[0].nValue);
        BOOST_CHECK(coinbaseKey.SignECDSA(hash, vchSig));
        vchSig.push_back(uint8_t(SIGHASH_ALL | SIGHASH_FORKID));
        spends[i].vin[0].scriptSig << vchSig;
    }

    CBlock block;

    // Test 1: block with both of those transactions should be rejected.
    block = CreateAndProcessBlock(spends, scriptPubKey);
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() != block.GetHash());

    // Test 2: ... and should be rejected if spend1 is in the memory pool
    BOOST_CHECK(ToMemPool(spends[0]));
    block = CreateAndProcessBlock(spends, scriptPubKey);
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() != block.GetHash());
    g_mempool.clear();

    // Test 3: ... and should be rejected if spend2 is in the memory pool
    BOOST_CHECK(ToMemPool(spends[1]));
    block = CreateAndProcessBlock(spends, scriptPubKey);
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() != block.GetHash());
    g_mempool.clear();

    // Final sanity test: first spend in mempool, second in block, that's OK:
    std::vector<CMutableTransaction> oneSpend;
    oneSpend.push_back(spends[0]);
    BOOST_CHECK(ToMemPool(spends[1]));
    block = CreateAndProcessBlock(oneSpend, scriptPubKey);
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() == block.GetHash());
    // spends[1] should have been removed from the mempool when the block with
    // spends[0] is accepted:
    BOOST_CHECK_EQUAL(g_mempool.size(), 0U);
}

// Run CheckInputs (using pcoinsTip) on the given transaction, for all script
// flags. Test that CheckInputs passes for all flags that don't overlap with the
// failing_flags argument, but otherwise fails.
// CHECKLOCKTIMEVERIFY and CHECKSEQUENCEVERIFY (and future NOP codes that may
// get reassigned) have an interaction with DISCOURAGE_UPGRADABLE_NOPS: if the
// script flags used contain DISCOURAGE_UPGRADABLE_NOPS but don't contain
// CHECKLOCKTIMEVERIFY (or CHECKSEQUENCEVERIFY), but the script does contain
// OP_CHECKLOCKTIMEVERIFY (or OP_CHECKSEQUENCEVERIFY), then script execution
// should fail.
// Capture this interaction with the upgraded_nop argument: set it when
// evaluating any script flag that is implemented as an upgraded NOP code.
static void
ValidateCheckInputsForAllFlags(const CTransaction &tx, uint32_t failing_flags,
                               uint32_t required_flags, bool add_to_cache)
    EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
    PrecomputedTransactionData txdata(tx);

    MMIXLinearCongruentialGenerator lcg;
    for (int i = 0; i < 4096; i++) {
        uint32_t test_flags = lcg.next() | required_flags;
        CValidationState state;

        // Filter out incompatible flag choices
        if ((test_flags & SCRIPT_VERIFY_CLEANSTACK)) {
            // CLEANSTACK requires P2SH, see VerifyScript() in
            // script/interpreter.cpp
            test_flags |= SCRIPT_VERIFY_P2SH;
        }

        bool ret = CheckInputs(tx, state, pcoinsTip.get(), true, test_flags,
                               true, add_to_cache, txdata, nullptr);

        // CheckInputs should succeed iff test_flags doesn't intersect with
        // failing_flags
        bool expected_return_value = !(test_flags & failing_flags);
        BOOST_CHECK_EQUAL(ret, expected_return_value);

        // Test the caching
        if (ret && add_to_cache) {
            // Check that we get a cache hit if the tx was valid
            std::vector<CScriptCheck> scriptchecks;
            BOOST_CHECK(CheckInputs(tx, state, pcoinsTip.get(), true,
                                    test_flags, true, add_to_cache, txdata,
                                    &scriptchecks));
            BOOST_CHECK(scriptchecks.empty());
        } else {
            // Check that we get script executions to check, if the transaction
            // was invalid, or we didn't add to cache.
            std::vector<CScriptCheck> scriptchecks;
            BOOST_CHECK(CheckInputs(tx, state, pcoinsTip.get(), true,
                                    test_flags, true, add_to_cache, txdata,
                                    &scriptchecks));
            BOOST_CHECK_EQUAL(scriptchecks.size(), tx.vin.size());
        }
    }
}

BOOST_FIXTURE_TEST_CASE(checkinputs_test, TestChain100Setup) {
    // Test that passing CheckInputs with one set of script flags doesn't imply
    // that we would pass again with a different set of flags.
    {
        LOCK(cs_main);
        InitScriptExecutionCache();
    }

    CScript p2pk_scriptPubKey =
        CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    CScript p2sh_scriptPubKey =
        GetScriptForDestination(CScriptID(p2pk_scriptPubKey));
    CScript p2pkh_scriptPubKey =
        GetScriptForDestination(coinbaseKey.GetPubKey().GetID());

    CBasicKeyStore keystore;
    keystore.AddKey(coinbaseKey);
    keystore.AddCScript(p2pk_scriptPubKey);

    CMutableTransaction funding_tx;
    // Needed when spending the output of this transaction
    CScript noppyScriptPubKey;
    // Create a transaction output that can fail DISCOURAGE_UPGRADABLE_NOPS
    // checks when spent. This is for testing consensus vs non-standard rules in
    // `checkinputs_test`.
    {
        funding_tx.nVersion = 1;
        funding_tx.vin.resize(1);
        funding_tx.vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetId(), 0);
        funding_tx.vout.resize(1);
        funding_tx.vout[0].nValue = 50 * COIN;

        noppyScriptPubKey << OP_IF << OP_NOP10 << OP_ENDIF << OP_1;
        funding_tx.vout[0].scriptPubKey = noppyScriptPubKey;
        std::vector<uint8_t> fundingVchSig;
        uint256 fundingSigHash = SignatureHash(
            p2pk_scriptPubKey, CTransaction(funding_tx), 0,
            SigHashType().withForkId(), m_coinbase_txns[0]->vout[0].nValue);
        BOOST_CHECK(coinbaseKey.SignECDSA(fundingSigHash, fundingVchSig));
        fundingVchSig.push_back(uint8_t(SIGHASH_ALL | SIGHASH_FORKID));
        funding_tx.vin[0].scriptSig << fundingVchSig;
    }

    // Spend the funding transaction by mining it into a block
    {
        CBlock block = CreateAndProcessBlock({funding_tx}, p2pk_scriptPubKey);
        BOOST_CHECK(chainActive.Tip()->GetBlockHash() == block.GetHash());
        BOOST_CHECK(pcoinsTip->GetBestBlock() == block.GetHash());
    }

    // flags to test: SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY,
    // SCRIPT_VERIFY_CHECKSEQUENCE_VERIFY,
    // SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS, uncompressed pubkey thing

    // Create 2 outputs that match the three scripts above, spending the first
    // coinbase tx.
    CMutableTransaction spend_tx;
    spend_tx.nVersion = 1;
    spend_tx.vin.resize(1);
    spend_tx.vin[0].prevout = COutPoint(funding_tx.GetId(), 0);
    spend_tx.vout.resize(4);
    spend_tx.vout[0].nValue = 11 * CENT;
    spend_tx.vout[0].scriptPubKey = p2sh_scriptPubKey;
    spend_tx.vout[1].nValue = 11 * CENT;
    spend_tx.vout[1].scriptPubKey =
        CScript() << OP_CHECKLOCKTIMEVERIFY << OP_DROP
                  << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    spend_tx.vout[2].nValue = 11 * CENT;
    spend_tx.vout[2].scriptPubKey =
        CScript() << OP_CHECKSEQUENCEVERIFY << OP_DROP
                  << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    spend_tx.vout[3].nValue = 11 * CENT;
    spend_tx.vout[3].scriptPubKey = p2sh_scriptPubKey;

    // "Sign" the main transaction that we spend from.
    {
        // This will cause OP_NOP10 to execute.
        spend_tx.vin[0].scriptSig << OP_1;
    }

    // Test that invalidity under a set of flags doesn't preclude validity under
    // other (eg consensus) flags.
    // spend_tx is invalid according to DISCOURAGE_UPGRADABLE_NOPS
    {
        const CTransaction tx(spend_tx);

        LOCK(cs_main);

        CValidationState state;
        PrecomputedTransactionData ptd_spend_tx(tx);

        BOOST_CHECK(!CheckInputs(tx, state, pcoinsTip.get(), true,
                                 STANDARD_SCRIPT_VERIFY_FLAGS, true, true,
                                 ptd_spend_tx, nullptr));

        // If we call again asking for scriptchecks (as happens in
        // ConnectBlock), we should add a script check object for this -- we're
        // not caching invalidity (if that changes, delete this test case).
        std::vector<CScriptCheck> scriptchecks;
        BOOST_CHECK(CheckInputs(tx, state, pcoinsTip.get(), true,
                                STANDARD_SCRIPT_VERIFY_FLAGS, true, true,
                                ptd_spend_tx, &scriptchecks));
        BOOST_CHECK_EQUAL(scriptchecks.size(), 1U);

        // Test that CheckInputs returns true iff cleanstack-enforcing flags are
        // not present. Don't add these checks to the cache, so that we can test
        // later that block validation works fine in the absence of cached
        // successes.
        ValidateCheckInputsForAllFlags(
            tx, SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS, 0, false);
    }

    // And if we produce a block with this tx, it should be valid, even though
    // there's no cache entry.
    CBlock block;

    block = CreateAndProcessBlock({spend_tx}, p2pk_scriptPubKey);
    LOCK(cs_main);
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() == block.GetHash());
    BOOST_CHECK(pcoinsTip->GetBestBlock() == block.GetHash());

    // Test P2SH: construct a transaction that is valid without P2SH, and then
    // test validity with P2SH.
    {
        CMutableTransaction invalid_under_p2sh_tx;
        invalid_under_p2sh_tx.nVersion = 1;
        invalid_under_p2sh_tx.vin.resize(1);
        invalid_under_p2sh_tx.vin[0].prevout = COutPoint(spend_tx.GetId(), 0);
        invalid_under_p2sh_tx.vout.resize(1);
        invalid_under_p2sh_tx.vout[0].nValue = 11 * CENT;
        invalid_under_p2sh_tx.vout[0].scriptPubKey = p2pk_scriptPubKey;
        std::vector<uint8_t> vchSig2(p2pk_scriptPubKey.begin(),
                                     p2pk_scriptPubKey.end());
        invalid_under_p2sh_tx.vin[0].scriptSig << vchSig2;

        ValidateCheckInputsForAllFlags(CTransaction(invalid_under_p2sh_tx),
                                       SCRIPT_VERIFY_P2SH, 0, true);
    }

    // Test CHECKLOCKTIMEVERIFY
    {
        CMutableTransaction invalid_with_cltv_tx;
        invalid_with_cltv_tx.nVersion = 1;
        invalid_with_cltv_tx.nLockTime = 100;
        invalid_with_cltv_tx.vin.resize(1);
        invalid_with_cltv_tx.vin[0].prevout = COutPoint(spend_tx.GetId(), 1);
        invalid_with_cltv_tx.vin[0].nSequence = 0;
        invalid_with_cltv_tx.vout.resize(1);
        invalid_with_cltv_tx.vout[0].nValue = 11 * CENT;
        invalid_with_cltv_tx.vout[0].scriptPubKey = p2pk_scriptPubKey;

        // Sign
        std::vector<uint8_t> vchSig;
        uint256 hash = SignatureHash(
            spend_tx.vout[1].scriptPubKey, CTransaction(invalid_with_cltv_tx),
            0, SigHashType().withForkId(), spend_tx.vout[1].nValue);
        BOOST_CHECK(coinbaseKey.SignECDSA(hash, vchSig));
        vchSig.push_back(uint8_t(SIGHASH_ALL | SIGHASH_FORKID));
        invalid_with_cltv_tx.vin[0].scriptSig = CScript() << vchSig << 101;

        ValidateCheckInputsForAllFlags(CTransaction(invalid_with_cltv_tx),
                                       SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY |
                                           SCRIPT_ENABLE_REPLAY_PROTECTION,
                                       SCRIPT_ENABLE_SIGHASH_FORKID, true);

        // Make it valid, and check again
        invalid_with_cltv_tx.vin[0].scriptSig = CScript() << vchSig << 100;
        CValidationState state;

        CTransaction transaction(invalid_with_cltv_tx);
        PrecomputedTransactionData txdata(transaction);

        BOOST_CHECK(CheckInputs(transaction, state, pcoinsTip.get(), true,
                                STANDARD_SCRIPT_VERIFY_FLAGS, true, true,
                                txdata, nullptr));
    }

    // TEST CHECKSEQUENCEVERIFY
    {
        CMutableTransaction invalid_with_csv_tx;
        invalid_with_csv_tx.nVersion = 2;
        invalid_with_csv_tx.vin.resize(1);
        invalid_with_csv_tx.vin[0].prevout = COutPoint(spend_tx.GetId(), 2);
        invalid_with_csv_tx.vin[0].nSequence = 100;
        invalid_with_csv_tx.vout.resize(1);
        invalid_with_csv_tx.vout[0].nValue = 11 * CENT;
        invalid_with_csv_tx.vout[0].scriptPubKey = p2pk_scriptPubKey;

        // Sign
        std::vector<uint8_t> vchSig;
        uint256 hash = SignatureHash(
            spend_tx.vout[2].scriptPubKey, CTransaction(invalid_with_csv_tx), 0,
            SigHashType().withForkId(), spend_tx.vout[2].nValue);
        BOOST_CHECK(coinbaseKey.SignECDSA(hash, vchSig));
        vchSig.push_back(uint8_t(SIGHASH_ALL | SIGHASH_FORKID));
        invalid_with_csv_tx.vin[0].scriptSig = CScript() << vchSig << 101;

        ValidateCheckInputsForAllFlags(CTransaction(invalid_with_csv_tx),
                                       SCRIPT_VERIFY_CHECKSEQUENCEVERIFY |
                                           SCRIPT_ENABLE_REPLAY_PROTECTION,
                                       SCRIPT_ENABLE_SIGHASH_FORKID, true);

        // Make it valid, and check again
        invalid_with_csv_tx.vin[0].scriptSig = CScript() << vchSig << 100;
        CValidationState state;

        CTransaction transaction(invalid_with_csv_tx);
        PrecomputedTransactionData txdata(transaction);

        BOOST_CHECK(CheckInputs(transaction, state, pcoinsTip.get(), true,
                                STANDARD_SCRIPT_VERIFY_FLAGS, true, true,
                                txdata, nullptr));
    }

    // TODO: add tests for remaining script flags

    {
        // Test a transaction with multiple inputs.
        CMutableTransaction tx;

        tx.nVersion = 1;
        tx.vin.resize(2);
        tx.vin[0].prevout = COutPoint(spend_tx.GetId(), 0);
        tx.vin[1].prevout = COutPoint(spend_tx.GetId(), 3);
        tx.vout.resize(1);
        tx.vout[0].nValue = 22 * CENT;
        tx.vout[0].scriptPubKey = p2pk_scriptPubKey;

        // Sign
        {
            SignatureData sigdata;
            BOOST_CHECK(ProduceSignature(
                keystore,
                MutableTransactionSignatureCreator(&tx, 0, 11 * CENT,
                                                   SigHashType().withForkId()),
                spend_tx.vout[0].scriptPubKey, sigdata));
            UpdateInput(tx.vin[0], sigdata);
        }
        {
            SignatureData sigdata;
            BOOST_CHECK(ProduceSignature(
                keystore,
                MutableTransactionSignatureCreator(&tx, 1, 11 * CENT,
                                                   SigHashType().withForkId()),
                spend_tx.vout[3].scriptPubKey, sigdata));
            UpdateInput(tx.vin[1], sigdata);
        }

        // This should be valid under all script flags that support our sighash
        // convention.
        ValidateCheckInputsForAllFlags(
            CTransaction(tx), SCRIPT_ENABLE_REPLAY_PROTECTION,
            SCRIPT_ENABLE_SIGHASH_FORKID | SCRIPT_VERIFY_P2SH, true);

        // Check that if the second input is invalid, but the first input is
        // valid, the transaction is not cached.
        // Invalidate vin[1]
        tx.vin[1].scriptSig = CScript();

        CValidationState state;
        CTransaction transaction(tx);
        PrecomputedTransactionData txdata(transaction);

        // This transaction is now invalid because the second signature is
        // missing.
        BOOST_CHECK(!CheckInputs(transaction, state, pcoinsTip.get(), true,
                                 STANDARD_SCRIPT_VERIFY_FLAGS, true, true,
                                 txdata, nullptr));

        // Make sure this transaction was not cached (ie becausethe first input
        // was valid)
        std::vector<CScriptCheck> scriptchecks;
        BOOST_CHECK(CheckInputs(transaction, state, pcoinsTip.get(), true,
                                STANDARD_SCRIPT_VERIFY_FLAGS, true, true,
                                txdata, &scriptchecks));
        // Should get 2 script checks back -- caching is on a whole-transaction
        // basis.
        BOOST_CHECK_EQUAL(scriptchecks.size(), 2U);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// ApertureCoin tapscript extensions: OP_CAT, 64-bit OP_MUL/DIV/MOD,
// transaction and token introspection, OP_CHECKTEMPLATEVERIFY.

#include <primitives/token.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(script_tapscript_ext_tests, BasicTestingSetup)

namespace {

using Stack = std::vector<std::vector<unsigned char>>;

struct Fixture {
    CMutableTransaction tx;
    std::vector<CTxOut> spent;
    PrecomputedTransactionData txdata;

    Fixture()
    {
        token::TokenData in_token{uint256S("aa"), token::NFT{token::Capability::MUTABLE, {0x01, 0x02}}, 500};
        spent.emplace_back(10 * COIN, token::Encode(in_token, CScript() << OP_TRUE));
        spent.emplace_back(3 * COIN, CScript() << OP_2);
        tx.nVersion = 2;
        tx.nLockTime = 777;
        tx.vin.resize(2);
        tx.vin[0].prevout = COutPoint(uint256S("01"), 0);
        tx.vin[0].nSequence = 0xfffffffe;
        tx.vin[1].prevout = COutPoint(uint256S("02"), 5);
        token::TokenData out_token{uint256S("aa"), std::nullopt, 400};
        tx.vout.emplace_back(9 * COIN, token::Encode(out_token, CScript() << OP_3));
        tx.vout.emplace_back(4 * COIN - 1000, CScript() << OP_4);
        txdata.Init(tx, std::vector<CTxOut>(spent));
    }

    bool Eval(const CScript& script, Stack& stack, ScriptError& err, bool with_context = true, unsigned int nIn = 0) const
    {
        ScriptExecutionData execdata;
        const MutableTransactionSignatureChecker checker(&tx, nIn, spent[nIn].nValue, txdata);
        const BaseSignatureChecker no_context;
        return EvalScript(stack, script, SCRIPT_VERIFY_MINIMALDATA, with_context ? static_cast<const BaseSignatureChecker&>(checker) : no_context,
                          SigVersion::TAPSCRIPT, execdata, &err);
    }

    /** Evaluate and return the single resulting stack item (hex). */
    std::string Top(const CScript& script, unsigned int nIn = 0) const
    {
        Stack stack;
        ScriptError err{SCRIPT_ERR_OK};
        const bool ok{Eval(script, stack, err, true, nIn)};
        BOOST_REQUIRE_MESSAGE(ok, ScriptErrorString(err));
        BOOST_REQUIRE_EQUAL(stack.size(), 1U);
        return HexStr(stack.back());
    }

    ScriptError Fail(const CScript& script, bool with_context = true) const
    {
        Stack stack;
        ScriptError err;
        BOOST_CHECK(!Eval(script, stack, err, with_context));
        return err;
    }
};

std::string Num(int64_t n) { return HexStr(CScriptNum(n).getvch()); }

} // namespace

BOOST_AUTO_TEST_CASE(not_op_success_anymore)
{
    for (opcodetype op : {OP_CAT, OP_MUL, OP_DIV, OP_MOD, OP_INPUTINDEX, OP_OUTPUTTOKENAMOUNT}) {
        BOOST_CHECK(!IsOpSuccess(op));
    }
    BOOST_CHECK(IsOpSuccess(static_cast<opcodetype>(0xca)));
    BOOST_CHECK(IsOpSuccess(static_cast<opcodetype>(0xd4)));
}

BOOST_AUTO_TEST_CASE(cat)
{
    Fixture f;
    BOOST_CHECK_EQUAL(f.Top(CScript() << std::vector<unsigned char>{0xab} << std::vector<unsigned char>{0xcd, 0xef} << OP_CAT), "abcdef");
    const std::vector<unsigned char> big(300, 0x11);
    BOOST_CHECK_EQUAL(f.Fail(CScript() << big << big << OP_CAT), SCRIPT_ERR_PUSH_SIZE);
}

BOOST_AUTO_TEST_CASE(arithmetic_64bit)
{
    Fixture f;
    const int64_t big{int64_t{1} << 40};
    BOOST_CHECK_EQUAL(f.Top(CScript() << big << 1000 << OP_MUL), Num(big * 1000));
    BOOST_CHECK_EQUAL(f.Top(CScript() << -7 << 2 << OP_DIV), Num(-3));
    BOOST_CHECK_EQUAL(f.Top(CScript() << -7 << 2 << OP_MOD), Num(-1));
    BOOST_CHECK_EQUAL(f.Top(CScript() << big << big << OP_ADD), Num(2 * big));
    BOOST_CHECK_EQUAL(f.Fail(CScript() << 5 << 0 << OP_DIV), SCRIPT_ERR_DIV_BY_ZERO);
    BOOST_CHECK_EQUAL(f.Fail(CScript() << 5 << 0 << OP_MOD), SCRIPT_ERR_DIV_BY_ZERO);
    const int64_t max{std::numeric_limits<int64_t>::max()};
    BOOST_CHECK_EQUAL(f.Fail(CScript() << max << 2 << OP_MUL), SCRIPT_ERR_NUMBER_OVERFLOW);
    BOOST_CHECK_EQUAL(f.Fail(CScript() << max << 1 << OP_ADD), SCRIPT_ERR_NUMBER_OVERFLOW);
    BOOST_CHECK_EQUAL(f.Fail(CScript() << max << OP_1ADD), SCRIPT_ERR_NUMBER_OVERFLOW);
}

BOOST_AUTO_TEST_CASE(transaction_introspection)
{
    Fixture f;
    BOOST_CHECK_EQUAL(f.Top(CScript() << OP_INPUTINDEX, 1), Num(1));
    BOOST_CHECK_EQUAL(f.Top(CScript() << OP_TXVERSION), Num(2));
    BOOST_CHECK_EQUAL(f.Top(CScript() << OP_TXINPUTCOUNT), Num(2));
    BOOST_CHECK_EQUAL(f.Top(CScript() << OP_TXOUTPUTCOUNT), Num(2));
    BOOST_CHECK_EQUAL(f.Top(CScript() << OP_TXLOCKTIME), Num(777));
    BOOST_CHECK_EQUAL(f.Top(CScript() << 1 << OP_UTXOVALUE), Num(3 * COIN));
    BOOST_CHECK_EQUAL(f.Top(CScript() << 0 << OP_UTXOBYTECODE), "51");      // token prefix stripped
    BOOST_CHECK_EQUAL(f.Top(CScript() << 1 << OP_OUTPOINTTXHASH), HexStr(uint256S("02")));
    BOOST_CHECK_EQUAL(f.Top(CScript() << 1 << OP_OUTPOINTINDEX), Num(5));
    BOOST_CHECK_EQUAL(f.Top(CScript() << 0 << OP_INPUTSEQUENCENUMBER), Num(0xfffffffe));
    BOOST_CHECK_EQUAL(f.Top(CScript() << 1 << OP_OUTPUTVALUE), Num(4 * COIN - 1000));
    BOOST_CHECK_EQUAL(f.Top(CScript() << 0 << OP_OUTPUTBYTECODE), "53");
    const CScript active{CScript() << OP_ACTIVEBYTECODE};
    BOOST_CHECK_EQUAL(f.Top(active), HexStr(active));
}

BOOST_AUTO_TEST_CASE(token_introspection)
{
    Fixture f;
    // Mutable NFT: category followed by the capability byte.
    BOOST_CHECK_EQUAL(f.Top(CScript() << 0 << OP_UTXOTOKENCATEGORY), HexStr(uint256S("aa")) + "01");
    BOOST_CHECK_EQUAL(f.Top(CScript() << 0 << OP_UTXOTOKENCOMMITMENT), "0102");
    BOOST_CHECK_EQUAL(f.Top(CScript() << 0 << OP_UTXOTOKENAMOUNT), Num(500));
    BOOST_CHECK_EQUAL(f.Top(CScript() << 0 << OP_OUTPUTTOKENCATEGORY), HexStr(uint256S("aa")));
    BOOST_CHECK_EQUAL(f.Top(CScript() << 0 << OP_OUTPUTTOKENCOMMITMENT), "");
    BOOST_CHECK_EQUAL(f.Top(CScript() << 0 << OP_OUTPUTTOKENAMOUNT), Num(400));
    // No tokens: empty category and zero amount.
    BOOST_CHECK_EQUAL(f.Top(CScript() << 1 << OP_UTXOTOKENCATEGORY), "");
    BOOST_CHECK_EQUAL(f.Top(CScript() << 1 << OP_OUTPUTTOKENAMOUNT), Num(0));
}

BOOST_AUTO_TEST_CASE(introspection_errors)
{
    Fixture f;
    BOOST_CHECK_EQUAL(f.Fail(CScript() << 2 << OP_UTXOVALUE), SCRIPT_ERR_INVALID_TX_INDEX);
    BOOST_CHECK_EQUAL(f.Fail(CScript() << -1 << OP_OUTPUTVALUE), SCRIPT_ERR_INVALID_TX_INDEX);
    BOOST_CHECK_EQUAL(f.Fail(CScript() << OP_OUTPUTVALUE), SCRIPT_ERR_INVALID_STACK_OPERATION);
    BOOST_CHECK_EQUAL(f.Fail(CScript() << OP_TXVERSION, /*with_context=*/false), SCRIPT_ERR_CONTEXT_NOT_PRESENT);

    // Outside tapscript the introspection opcodes are invalid.
    Stack stack;
    ScriptError err;
    BOOST_CHECK(!EvalScript(stack, CScript() << OP_TXVERSION, 0, BaseSignatureChecker(), SigVersion::WITNESS_V0, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_BAD_OPCODE);
}

BOOST_AUTO_TEST_CASE(checktemplateverify)
{
    Fixture f;
    ScriptIntrospection ctx;
    ctx.version = f.tx.nVersion;
    ctx.locktime = f.tx.nLockTime;
    ctx.input_index = 0;
    ctx.vin = &f.tx.vin;
    ctx.vout = &f.tx.vout;
    const uint256 hash{ComputeStandardTemplateHash(ctx)};
    const std::vector<unsigned char> hash_bytes(hash.begin(), hash.end());
    BOOST_CHECK_EQUAL(f.Top(CScript() << hash_bytes << OP_CHECKTEMPLATEVERIFY), HexStr(hash_bytes));
    // Committing to input index 1 does not match input 0.
    ctx.input_index = 1;
    const uint256 other{ComputeStandardTemplateHash(ctx)};
    BOOST_CHECK_EQUAL(f.Fail(CScript() << std::vector<unsigned char>(other.begin(), other.end()) << OP_CHECKTEMPLATEVERIFY),
                      SCRIPT_ERR_TEMPLATE_MISMATCH);
    // Non-32-byte arguments are upgradable NOPs.
    BOOST_CHECK_EQUAL(f.Top(CScript() << std::vector<unsigned char>{0x01, 0x02} << OP_CHECKTEMPLATEVERIFY), "0102");
}

BOOST_AUTO_TEST_SUITE_END()

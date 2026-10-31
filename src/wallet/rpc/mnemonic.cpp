// Copyright (c) 2026 The Freicoin developers
// Distributed under the terms of the GNU Affero General Public License v3.
//
// 12-word (BIP39) recovery phrases for descriptor wallets.
//
// Derivation matches the freicoin.ru web wallet (github.com/denisov-k/freicoin-wallet):
// BIP39 English, no BIP39 passphrase, BIP32 "Bitcoin seed", BIP84 account
// m/84'/coin'/0' (coin 0 on main, 1 on test chains), receive /0/*, change /1/*,
// wpk() outputs (fc1q... addresses).

#include <chainparams.h>
#include <key.h>
#include <key_io.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <support/cleanse.h>
#include <util/strencodings.h>
#include <wallet/bip39.h>
#include <wallet/rpc/util.h>
#include <wallet/wallet.h>

#include <univalue.h>

namespace wallet {

RPCHelpMan importdescriptors();

namespace {

struct MnemonicDescriptors {
    SecureString recv_priv, change_priv; // with checksum
    std::string recv_pub, change_pub;    // with checksum
};

MnemonicDescriptors DescriptorsFromMnemonic(const SecureString& normalized)
{
    bip39::SecureBytes seed = bip39::MnemonicToSeed(normalized);
    CExtKey master;
    master.SetSeed(MakeByteSpan(seed));

    const unsigned int coin = Params().IsTestChain() ? 1 : 0;
    CExtKey purpose, cointype, account;
    if (!master.Derive(purpose, 84 | 0x80000000) ||
        !purpose.Derive(cointype, coin | 0x80000000) ||
        !cointype.Derive(account, 0 | 0x80000000)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Key derivation failed");
    }
    const CKeyID master_id = master.key.GetPubKey().GetID();
    const std::string origin = "[" + HexStr(Span{master_id}.first(4)) + strprintf("/84h/%uh/0h]", coin);

    std::string xprv = EncodeExtKey(account);
    const std::string xpub = EncodeExtPubKey(account.Neuter());

    MnemonicDescriptors d;
    for (int chain = 0; chain < 2; ++chain) {
        std::string priv = "wpk(" + origin + xprv + strprintf("/%d/*)", chain);
        std::string pub = "wpk(" + origin + xpub + strprintf("/%d/*)", chain);
        priv += "#" + GetDescriptorChecksum(priv);
        pub += "#" + GetDescriptorChecksum(pub);
        (chain == 0 ? d.recv_priv : d.change_priv) = SecureString(priv.begin(), priv.end());
        (chain == 0 ? d.recv_pub : d.change_pub) = pub;
        memory_cleanse(priv.data(), priv.size());
    }
    memory_cleanse(xprv.data(), xprv.size());
    return d;
}

SecureString CheckedMnemonic(const UniValue& v)
{
    SecureString in(v.get_str().begin(), v.get_str().end());
    auto norm = bip39::NormalizeAndCheck(in);
    if (!norm) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid recovery phrase (unknown word, wrong word count, or checksum mismatch)");
    }
    return *norm;
}

} // namespace

RPCHelpMan generatemnemonic()
{
    return RPCHelpMan{"generatemnemonic",
        "\nGenerate a new BIP39 recovery phrase from the strong random number generator.\n"
        "Nothing is stored. Use importmnemonic on a blank descriptor wallet to use it.\n",
        {
            {"words", RPCArg::Type::NUM, RPCArg::Default{12}, "Number of words: 12, 15, 18, 21 or 24"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR, "mnemonic", "The recovery phrase. Write it down and keep it secret."},
        }},
        RPCExamples{HelpExampleCli("generatemnemonic", "") + HelpExampleRpc("generatemnemonic", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const int words = request.params[0].isNull() ? 12 : request.params[0].getInt<int>();
    if (words != 12 && words != 15 && words != 18 && words != 21 && words != 24) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "words must be 12, 15, 18, 21 or 24");
    }
    SecureString m = bip39::GenerateMnemonic(words);
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("mnemonic", std::string(m.begin(), m.end()));
    return obj;
},
    };
}

RPCHelpMan mnemonicdescriptors()
{
    return RPCHelpMan{"mnemonicdescriptors",
        "\nShow the public descriptors a BIP39 recovery phrase produces (m/84'/coin'/0', wpk).\n"
        "Use deriveaddresses on them to preview addresses. Nothing is stored.\n",
        {
            {"mnemonic", RPCArg::Type::STR, RPCArg::Optional::NO, "The recovery phrase"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR, "receive", "Public receive descriptor"},
            {RPCResult::Type::STR, "change", "Public change descriptor"},
        }},
        RPCExamples{HelpExampleCli("mnemonicdescriptors", "\"word1 word2 ... word12\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const MnemonicDescriptors d = DescriptorsFromMnemonic(CheckedMnemonic(request.params[0]));
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("receive", d.recv_pub);
    obj.pushKV("change", d.change_pub);
    return obj;
},
    };
}

RPCHelpMan importmnemonic()
{
    return RPCHelpMan{"importmnemonic",
        "\nMake a BIP39 recovery phrase the source of this descriptor wallet's addresses.\n"
        "Imports wpk(m/84'/coin'/0'/0/*) and /1/* as the active receive and change descriptors\n"
        "(compatible with the freicoin.ru wallet) and rescans from the given time.\n"
        "Intended for a new blank descriptor wallet.\n",
        {
            {"mnemonic", RPCArg::Type::STR, RPCArg::Optional::NO, "The recovery phrase"},
            {"timestamp", RPCArg::Type::NUM, RPCArg::Default{0}, "Rescan from this time (" + UNIX_EPOCH_TIME + "). Use \"now\" for a brand new phrase, 0 to restore (full rescan).",
                RPCArgOptions{.skip_type_check=true, .type_str={"timestamp | \"now\"", "integer / string"}}},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::BOOL, "success", "Both descriptors imported"},
            {RPCResult::Type::STR, "receive", "Public receive descriptor"},
            {RPCResult::Type::STR, "change", "Public change descriptor"},
            {RPCResult::Type::ARR, "result", "importdescriptors result", {{RPCResult::Type::ELISION, "", ""}}},
        }},
        RPCExamples{HelpExampleCli("importmnemonic", "\"word1 word2 ... word12\" now")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    if (!pwallet->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "importmnemonic needs a descriptor wallet");
    }
    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "importmnemonic needs a wallet with private keys enabled");
    }

    const MnemonicDescriptors d = DescriptorsFromMnemonic(CheckedMnemonic(request.params[0]));
    UniValue ts{0};
    if (!request.params[1].isNull()) {
        const UniValue& t = request.params[1];
        if (t.isNum()) {
            ts = t;
        } else if (t.isStr() && t.get_str() == "now") {
            ts = t;
        } else if (t.isStr()) {
            const auto n = ToIntegral<int64_t>(t.get_str());
            if (!n) throw JSONRPCError(RPC_INVALID_PARAMETER, "timestamp must be a number or \"now\"");
            ts = UniValue(*n);
        } else {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "timestamp must be a number or \"now\"");
        }
    }

    UniValue reqs(UniValue::VARR);
    for (int chain = 0; chain < 2; ++chain) {
        const SecureString& desc = chain == 0 ? d.recv_priv : d.change_priv;
        UniValue r(UniValue::VOBJ);
        r.pushKV("desc", std::string(desc.begin(), desc.end()));
        r.pushKV("active", true);
        r.pushKV("timestamp", ts);
        r.pushKV("internal", chain == 1);
        reqs.push_back(r);
    }
    JSONRPCRequest sub = request;
    sub.strMethod = "importdescriptors";
    sub.params = UniValue(UniValue::VARR);
    sub.params.push_back(reqs);
    const UniValue res = importdescriptors().HandleRequest(sub);

    bool ok = res.isArray() && res.size() == 2;
    for (size_t i = 0; ok && i < res.size(); ++i) ok = res[i]["success"].get_bool();

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("success", ok);
    obj.pushKV("receive", d.recv_pub);
    obj.pushKV("change", d.change_pub);
    obj.pushKV("result", res);
    return obj;
},
    };
}

} // namespace wallet

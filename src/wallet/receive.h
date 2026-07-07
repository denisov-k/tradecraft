// Copyright (c) 2021-2022 The Bitcoin Core developers
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

#ifndef FREICOIN_WALLET_RECEIVE_H
#define FREICOIN_WALLET_RECEIVE_H

#include <consensus/amount.h>
#include <primitives/transaction_identifier.h>
#include <wallet/transaction.h>
#include <wallet/wallet.h>

namespace wallet {
bool InputIsMine(const CWallet& wallet, const CTxIn& txin) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);

/** Returns whether all of the inputs belong to the wallet*/
bool AllInputsMine(const CWallet& wallet, const CTransaction& tx);

CAmount OutputGetCredit(const CWallet& wallet, const CTxOut& txout);
CAmount TxGetCredit(const CWallet& wallet, const CTransaction& tx);

bool ScriptIsChange(const CWallet& wallet, const CScript& script) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);
bool OutputIsChange(const CWallet& wallet, const CTxOut& txout) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);
CAmount OutputGetChange(const CWallet& wallet, const CTxOut& txout) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);
CAmount TxGetChange(const CWallet& wallet, const CTransaction& tx);

CAmount CachedTxGetCredit(const CWallet& wallet, const CWalletTx& wtx, bool avoid_reuse)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);
CAmount CachedTxGetDebit(const CWallet& wallet, const CWalletTx& wtx, bool avoid_reuse);
CAmount CachedTxGetChange(const CWallet& wallet, const CWalletTx& wtx);
struct COutputEntry
{
    CTxDestination destination;
    CAmount amount;
    uint32_t refheight;
    int vout;
};
void CachedTxGetAmounts(const CWallet& wallet, const CWalletTx& wtx,
                        std::list<COutputEntry>& listReceived,
                        std::list<COutputEntry>& listSent,
                        CAmount& nFee, CAmount& demurrage,
                        bool include_change);
bool CachedTxIsFromMe(const CWallet& wallet, const CWalletTx& wtx);
bool CachedTxIsTrusted(const CWallet& wallet, const CWalletTx& wtx, std::set<Txid>& trusted_parents) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);
bool CachedTxIsTrusted(const CWallet& wallet, const CWalletTx& wtx);

struct Balance {
    CAmount m_mine_trusted{0};           //!< Trusted, at depth=GetBalance.min_depth or more
    CAmount m_mine_untrusted_pending{0}; //!< Untrusted, but in mempool (pending)
    CAmount m_mine_immature{0};          //!< Immature coinbases in the main chain
};
Balance GetBalance(const CWallet& wallet, int min_depth = 0, bool avoid_reuse = true);

std::map<CTxDestination, CAmount> GetAddressBalances(const CWallet& wallet);
std::set<std::set<CTxDestination>> GetAddressGroupings(const CWallet& wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);
} // namespace wallet

#endif // FREICOIN_WALLET_RECEIVE_H

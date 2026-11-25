------------------------------ MODULE WAccStartIgnoresAI ------------------------------
\* Reversed witness: a driver starts limited only for an account billed at API rate, whatever its AI's limit. NeverWeakerThanAI must reject it.
EXTENDS MCAccounts

BrokenStart(s) == StartWith(LAMBDA x : ses[x].acct /= None /\ bill[ses[x].acct] = "api", s)

BrokenSpec == Init /\ [][NextLimitWith(LinkAccount, AddAI, BrokenStart, ReloadLimit, ReloadBilling)]_vars
=============================================================================

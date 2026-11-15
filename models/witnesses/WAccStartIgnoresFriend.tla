------------------------------ MODULE WAccStartIgnoresFriend ------------------------------
\* Reversed witness: a driver starts limited only for an account billed at API rate, whatever its friend's limit. NeverWeakerThanFriend must reject it.
EXTENDS MCAccounts

BrokenStart(s) == StartWith(LAMBDA x : ses[x].acct /= None /\ bill[ses[x].acct] = "api", s)

BrokenSpec == Init /\ [][NextLimitWith(LinkAccount, AddFriend, BrokenStart, ReloadLimit, ReloadBilling)]_vars
=============================================================================

------------------------------ MODULE WAccStartIgnoresBilling ------------------------------
\* Reversed witness: a driver starts limited only for a limited friend, whatever its account's billing, and a friend not limited may link an account billed at API rate. ApiAlwaysLimited must reject it.
EXTENDS MCAccounts

BrokenStart(s) == StartWith(LAMBDA x : lim[ses[x].friend], s)

BrokenLink(f, a) ==
    IF f \in known /\ acct[a] /= "absent" /\ a \notin Range(links[f])
    THEN Become("link", f, a, acct, prov, known, [links EXCEPT ![f] = Append(@, a)], dflt)
    ELSE Refused("link", f, a, 0, {})

BrokenSpec == Init /\ [][NextLimitWith(BrokenLink, AddFriend, BrokenStart, ReloadLimit, ReloadBilling)]_vars
=============================================================================

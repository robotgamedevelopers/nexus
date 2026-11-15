------------------------------ MODULE WAccLinkApiUnlimited ------------------------------
\* Reversed witness: a friend not limited may link an account billed at API rate. LimitSound must reject it.
EXTENDS MCAccounts

BrokenLink(f, a) ==
    IF f \in known /\ acct[a] /= "absent" /\ a \notin Range(links[f])
    THEN Become("link", f, a, acct, prov, known, [links EXCEPT ![f] = Append(@, a)], dflt)
    ELSE Refused("link", f, a, 0, {})

BrokenSpec == Init /\ [][NextLimitWith(BrokenLink, AddFriend, Start, ReloadLimit, ReloadBilling)]_vars
=============================================================================

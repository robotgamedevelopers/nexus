------------------------------ MODULE WAccUnlinkKeepsDefault ------------------------------
\* Reversed witness: unlinking the AI's default leaves it the default. DefaultLinked must reject it.
EXTENDS MCAccounts

BrokenUnlink(f, a) ==
    IF a \in Range(links[f])
    THEN Become("unlink", f, a, acct, prov, known, [links EXCEPT ![f] = Drop(@, a)], dflt)
    ELSE Refused("unlink", f, a, 0, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, BrokenUnlink, SetDefault, SetSession, Rotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

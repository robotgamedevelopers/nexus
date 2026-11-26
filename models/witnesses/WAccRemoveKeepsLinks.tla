------------------------------ MODULE WAccRemoveKeepsLinks ------------------------------
\* Reversed witness: removing an account leaves the AIs' links to it. LinksSound must reject it.
EXTENDS MCAccounts

BrokenRemove(a) ==
    IF acct[a] /= "absent"
    THEN Become("remove", None, a, [acct EXCEPT ![a] = "absent"], prov, known, links,
                [f \in AIs |-> IF dflt[f] = a THEN None ELSE dflt[f]])
    ELSE Refused("remove", None, a, 0, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, BrokenRemove, Unlink, SetDefault, SetSession, Rotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

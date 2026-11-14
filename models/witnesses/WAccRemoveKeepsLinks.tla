------------------------------ MODULE WAccRemoveKeepsLinks ------------------------------
\* Reversed witness: removing an account leaves the friends' links to it. LinksSound must reject it.
EXTENDS MCAccounts

BrokenRemove(a) ==
    IF acct[a] /= "absent"
    THEN LET S == On(a) IN
         /\ acct' = [acct EXCEPT ![a] = "absent"]
         /\ dflt' = [f \in Friends |-> IF dflt[f] = a THEN None ELSE dflt[f]]
         /\ ses' = MoveOff(a, S)
         /\ told' = S
         /\ Done("remove", None, a, 0, TRUE)
         /\ UNCHANGED <<known, links, nextSes>>
    ELSE Refused("remove", None, a, 0, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, BrokenRemove, Unlink, SetDefault, SetSession, Rotate, Create)]_vars
=============================================================================

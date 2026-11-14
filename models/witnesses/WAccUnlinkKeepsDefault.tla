------------------------------ MODULE WAccUnlinkKeepsDefault ------------------------------
\* Reversed witness: unlinking the friend's default leaves it the default. DefaultLinked must reject it.
EXTENDS MCAccounts

BrokenUnlink(f, a) ==
    IF a \in Range(links[f])
    THEN LET S == {s \in On(a) : ses[s].friend = f} IN
         /\ links' = [links EXCEPT ![f] = Drop(@, a)]
         /\ ses' = MoveOff(a, S)
         /\ told' = S
         /\ Done("unlink", f, a, 0, TRUE)
         /\ UNCHANGED <<acct, known, dflt, nextSes>>
    ELSE Refused("unlink", f, a, 0, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, BrokenUnlink, SetDefault, SetSession, Rotate, Create)]_vars
=============================================================================

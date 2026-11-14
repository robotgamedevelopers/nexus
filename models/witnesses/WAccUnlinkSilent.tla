------------------------------ MODULE WAccUnlinkSilent ------------------------------
\* Reversed witness: unlinking moves the friend's sessions off the account but tells none of them. MovedIsTold must reject it.
EXTENDS MCAccounts

BrokenUnlink(f, a) ==
    IF a \in Range(links[f])
    THEN LET S == {s \in On(a) : ses[s].friend = f} IN
         /\ links' = [links EXCEPT ![f] = Drop(@, a)]
         /\ dflt' = [dflt EXCEPT ![f] = IF @ = a THEN None ELSE @]
         /\ ses' = MoveOff(a, S)
         /\ told' = {}
         /\ Done("unlink", f, a, 0, TRUE)
         /\ UNCHANGED <<acct, known, nextSes>>
    ELSE Refused("unlink", f, a, 0, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, BrokenUnlink, SetDefault, SetSession, Rotate, Create)]_vars
=============================================================================

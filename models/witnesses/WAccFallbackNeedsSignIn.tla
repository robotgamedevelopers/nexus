------------------------------ MODULE WAccFallbackNeedsSignIn ------------------------------
\* Reversed witness: a session moved off an account goes to the friend's default whether or not it is signed in. ServerPicksSignedIn must reject it.
EXTENDS MCAccounts

BrokenFallback(f, x) == IF dflt[f] \notin {x, None} THEN dflt[f] ELSE NextUp(f, x)
BrokenMoveOff(x, S) == [s \in Ids |-> IF s \in S
                                      THEN LET n == BrokenFallback(ses[s].friend, x) IN
                                           [ses[s] EXCEPT !.acct = n, !.harness = HarnessFor(n, @)]
                                      ELSE ses[s]]
BrokenUnlink(f, a) ==
    IF a \in Range(links[f])
    THEN LET S == {s \in On(a) : ses[s].friend = f} IN
         /\ links' = [links EXCEPT ![f] = Drop(@, a)]
         /\ dflt' = [dflt EXCEPT ![f] = IF @ = a THEN None ELSE @]
         /\ ses' = BrokenMoveOff(a, S)
         /\ told' = S
         /\ Done("unlink", f, a, 0, TRUE)
         /\ UNCHANGED <<acct, known, nextSes>>
    ELSE Refused("unlink", f, a, 0, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, BrokenUnlink, SetDefault, SetSession, Rotate, Create)]_vars
=============================================================================

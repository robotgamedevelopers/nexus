------------------------------ MODULE WAccRotateAny ------------------------------
\* Reversed witness: rotation takes the next linked account whether or not it is signed in. ServerPicksSignedIn must reject it.
EXTENDS MCAccounts

AnyNext(f, x) == LET c == After(links[f], x) IN IF c = << >> THEN None ELSE Head(c)
BrokenRotate(s) ==
    IF ses[s].made
    THEN LET n == AnyNext(ses[s].friend, ses[s].acct) IN
         IF n = None
         THEN Refused("rotate", None, None, s, {s})
         ELSE /\ ses' = [ses EXCEPT ![s].acct = n, ![s].harness = prov[n]]
              /\ Done("rotate", None, n, s, TRUE) /\ told' = {s}
              /\ UNCHANGED <<insts, acct, prov, known, links, dflt, nextSes>>
    ELSE Refused("rotate", None, None, s, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, BrokenRotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

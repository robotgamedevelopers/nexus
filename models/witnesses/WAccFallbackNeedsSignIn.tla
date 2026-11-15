------------------------------ MODULE WAccFallbackNeedsSignIn ------------------------------
\* Reversed witness: a session moved off an account goes to the friend's default whether or not it is signed in. ServerPicksSignedIn must reject it.
EXTENDS MCAccounts

BrokenLanding(nl, nd, f) == IF nd[f] /= None THEN nd[f] ELSE FirstUp(nl[f])
BrokenSettled(nl, nd, np) ==
    [s \in Ids |-> IF Stray(nl, s)
                   THEN LET n == BrokenLanding(nl, nd, ses[s].friend) IN
                        [ses[s] EXCEPT !.acct = n, !.harness = HarnessFor(n, np, @)]
                   ELSE ses[s]]
BrokenUnlink(f, a) ==
    IF a \in Range(links[f])
    THEN LET nl == [links EXCEPT ![f] = Drop(@, a)]
             nd == [dflt EXCEPT ![f] = IF @ = a THEN None ELSE @] IN
         /\ links' = nl /\ dflt' = nd
         /\ ses' = BrokenSettled(nl, nd, prov)
         /\ told' = SettledTold(nl, prov)
         /\ Done("unlink", f, a, 0, TRUE)
         /\ UNCHANGED <<lim, bill, run, insts, acct, prov, known, nextSes>>
    ELSE Refused("unlink", f, a, 0, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, BrokenUnlink, SetDefault, SetSession, Rotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

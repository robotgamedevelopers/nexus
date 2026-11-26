------------------------------ MODULE WAccUnlinkSilent ------------------------------
\* Reversed witness: unlinking moves the AI's sessions off the account but tells none of them. MovedIsTold must reject it.
EXTENDS MCAccounts

BrokenUnlink(f, a) ==
    IF a \in Range(links[f])
    THEN LET nl == [links EXCEPT ![f] = Drop(@, a)]
             nd == [dflt EXCEPT ![f] = IF @ = a THEN None ELSE @] IN
         /\ links' = nl /\ dflt' = nd
         /\ ses' = Settled(nl, nd, prov)
         /\ told' = {}
         /\ Done("unlink", f, a, 0, TRUE)
         /\ UNCHANGED <<lim, bill, run, insts, acct, prov, known, nextSes>>
    ELSE Refused("unlink", f, a, 0, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, BrokenUnlink, SetDefault, SetSession, Rotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

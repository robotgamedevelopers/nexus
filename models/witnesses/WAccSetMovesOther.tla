------------------------------ MODULE WAccSetMovesOther ------------------------------
\* Reversed witness: setting a session's account sets it on every session of its AI. OnlyItsSession must reject it.
EXTENDS MCAccounts

BrokenSet(s, a) ==
    IF ses[s].made /\ (a = None \/ a \in Range(links[ses[s].ai]))
    THEN /\ ses' = [t \in Ids |-> IF ses[t].made /\ ses[t].ai = ses[s].ai
                                  THEN [ses[t] EXCEPT !.acct = a, !.harness = HarnessFor(a, prov, @)] ELSE ses[t]]
         /\ Done("set", None, a, s, TRUE) /\ told' = {}
         /\ UNCHANGED <<lim, bill, run, insts, acct, prov, known, links, dflt, nextSes>>
    ELSE Refused("set", None, a, s, IF ses[s].made THEN {s} ELSE {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, BrokenSet, Rotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

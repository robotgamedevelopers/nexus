------------------------------ MODULE WAccSetUnlinked ------------------------------
\* Reversed witness: the person may set a session to any account set up, linked to its friend or not. SessionLinked must reject it.
EXTENDS MCAccounts

BrokenSet(s, a) ==
    IF ses[s].made /\ (a = None \/ acct[a] /= "absent")
    THEN /\ ses' = [ses EXCEPT ![s].acct = a, ![s].harness = HarnessFor(a, prov, @)]
         /\ Done("set", None, a, s, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, prov, known, links, dflt, nextSes>>
    ELSE Refused("set", None, a, s, IF ses[s].made THEN {s} ELSE {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, BrokenSet, Rotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

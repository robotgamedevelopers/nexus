------------------------------ MODULE WAccRemoveStays ------------------------------
\* Reversed witness: removing an account leaves the sessions on it where they are. SessionLinked must reject it.
EXTENDS MCAccounts

BrokenRemove(a) ==
    IF acct[a] /= "absent"
    THEN /\ acct' = [acct EXCEPT ![a] = "absent"]
         /\ links' = [f \in Friends |-> Drop(links[f], a)]
         /\ dflt' = [f \in Friends |-> IF dflt[f] = a THEN None ELSE dflt[f]]
         /\ told' = {}
         /\ Done("remove", None, a, 0, TRUE)
         /\ UNCHANGED <<insts, prov, known, ses, nextSes>>
    ELSE Refused("remove", None, a, 0, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, BrokenRemove, Unlink, SetDefault, SetSession, Rotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

------------------------------ MODULE WAccDefaultUnlinked ------------------------------
\* Reversed witness: a friend's default may be any account set up, linked or not. DefaultLinked must reject it.
EXTENDS MCAccounts

BrokenDefault(f, a) ==
    IF f \in known /\ (a = None \/ acct[a] /= "absent")
    THEN /\ dflt' = [dflt EXCEPT ![f] = a]
         /\ Done("default", f, a, 0, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, known, links, ses, nextSes>>
    ELSE Refused("default", f, a, 0, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, BrokenDefault, SetSession, Rotate, Create)]_vars
=============================================================================

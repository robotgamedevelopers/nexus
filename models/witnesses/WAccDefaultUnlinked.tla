------------------------------ MODULE WAccDefaultUnlinked ------------------------------
\* Reversed witness: an AI's default may be any account set up, linked or not. DefaultLinked must reject it.
EXTENDS MCAccounts

BrokenDefault(f, a) ==
    IF f \in known /\ (a = None \/ acct[a] /= "absent")
    THEN Become("default", f, a, acct, prov, known, links, [dflt EXCEPT ![f] = a])
    ELSE Refused("default", f, a, 0, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, BrokenDefault, SetSession, Rotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

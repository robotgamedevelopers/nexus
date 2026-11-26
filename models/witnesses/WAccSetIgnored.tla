------------------------------ MODULE WAccSetIgnored ------------------------------
\* Reversed witness: a set to a linked account is taken but changes nothing. SetExact must reject it.
EXTENDS MCAccounts

BrokenSet(s, a) ==
    IF ses[s].made /\ (a = None \/ a \in Range(links[ses[s].ai]))
    THEN /\ Done("set", None, a, s, TRUE) /\ told' = {}
         /\ UNCHANGED model
    ELSE Refused("set", None, a, s, IF ses[s].made THEN {s} ELSE {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, BrokenSet, Rotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

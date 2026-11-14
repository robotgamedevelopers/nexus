------------------------------ MODULE WAccCrossKeepsHarness ------------------------------
\* Reversed witness: a switch to another provider's account keeps the old harness. HarnessMatches must reject it.
EXTENDS MCAccounts

BrokenSet(s, a) ==
    IF ses[s].made /\ (a = None \/ a \in Range(links[ses[s].friend]))
    THEN /\ ses' = [ses EXCEPT ![s].acct = a]
         /\ Done("set", None, a, s, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, known, links, dflt, nextSes>>
    ELSE Refused("set", None, a, s, IF ses[s].made THEN {s} ELSE {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, BrokenSet, Rotate, Create)]_vars
=============================================================================

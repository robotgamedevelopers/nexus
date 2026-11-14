------------------------------ MODULE WAccCreateCopiesForeign ------------------------------
\* Reversed witness: a create copies the account of the session it names, even another friend's. SessionLinked must reject it.
EXTENDS MCAccounts

BrokenCreate(f, from) ==
    IF nextSes <= MaxSes /\ (from = 0 \/ ses[from].made)
    THEN LET a == IF from /= 0 THEN ses[from].acct ELSE dflt[f] IN
         /\ ses' = [ses EXCEPT ![nextSes] = [made |-> TRUE, friend |-> f, acct |-> a,
                                             harness |-> HarnessFor(a, StartHarness)]]
         /\ nextSes' = nextSes + 1
         /\ Done("create", f, a, from, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, known, links, dflt>>
    ELSE Refused("create", f, None, from, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, Rotate, BrokenCreate)]_vars
=============================================================================

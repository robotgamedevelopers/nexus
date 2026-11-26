------------------------------ MODULE WAccCreateCopiesForeign ------------------------------
\* Reversed witness: a create copies the account of the session it names, even another AI's. SessionLinked must reject it.
EXTENDS MCAccounts

BrokenCreate(f, from) ==
    IF nextSes <= MaxSes /\ (from = 0 \/ ses[from].made)
    THEN LET a == IF from /= 0 THEN ses[from].acct ELSE dflt[f] IN
         /\ ses' = [ses EXCEPT ![nextSes] = [made |-> TRUE, ai |-> f, inst |-> FirstOr(insts[f]), acct |-> a,
                                             harness |-> HarnessFor(a, prov, StartHarness), automatic |-> FALSE]]
         /\ nextSes' = nextSes + 1
         /\ Done("create", f, a, from, TRUE) /\ told' = {}
         /\ UNCHANGED <<lim, bill, run, insts, acct, prov, known, links, dflt>>
    ELSE Refused("create", f, None, from, {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, Rotate, BrokenCreate, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

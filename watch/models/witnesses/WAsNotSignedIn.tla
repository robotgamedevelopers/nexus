--------------------------- MODULE WAsNotSignedIn ---------------------------
\* Reversed witness: the watch lists, and lets the person choose, an account that is not signed in. OnlySignedIn must reject it.
EXTENDS AccountSwitch

BrokenChoose(a) ==
    /\ Cardinality(chosen) < MaxChoice
    /\ shown # ""
    /\ a # Target
    /\ choice' = [num |-> Cardinality(chosen) + 1, acct |-> a]
    /\ chosen' = chosen \cup {choice'}
    /\ UNCHANGED <<server, applied, shown, reqs, resps, phoneVars, linkQ, crashes>>

BrokenSpec == Init /\ [][Next \/ \E a \in Accounts : BrokenChoose(a)]_vars
=============================================================================

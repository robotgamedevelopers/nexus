-------------------------- MODULE WAsSwitchCurrent --------------------------
\* Reversed witness: a choice of the account the AI is on already is sent anyway. ChoiceChanges must reject it.
EXTENDS AccountSwitch

BrokenChoose(a) ==
    /\ Cardinality(chosen) < MaxChoice
    /\ shown # ""
    /\ a \in SignedIn
    /\ choice' = [num |-> Cardinality(chosen) + 1, acct |-> a]
    /\ chosen' = chosen \cup {choice'}
    /\ UNCHANGED <<server, applied, shown, reqs, resps, phoneVars, linkQ, crashes>>

BrokenSpec == Init /\ [][Next \/ \E a \in Accounts : BrokenChoose(a)]_vars
=============================================================================

------------------------------ MODULE WAccLimitSilent ------------------------------
\* Reversed witness: a friend's limit changed tells none of its sessions. LimitChangeIsTold must reject it.
EXTENDS MCAccounts

BrokenLimit(f, v) ==
    IF f \in known /\ ConsistentLB(acct, known, links, dflt, insts, [lim EXCEPT ![f] = v], bill)
    THEN /\ lim' = [lim EXCEPT ![f] = v]
         /\ told' = {}
         /\ Done("limit", f, None, 0, TRUE)
         /\ UNCHANGED <<acct, prov, known, links, dflt, insts, ses, nextSes, bill, run>>
    ELSE Refused("limit", f, None, 0, {})

BrokenSpec == Init /\ [][NextLimitWith(LinkAccount, AddFriend, Start, BrokenLimit, ReloadBilling)]_vars
=============================================================================

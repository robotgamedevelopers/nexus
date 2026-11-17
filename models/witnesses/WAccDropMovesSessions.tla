------------------------------ MODULE WAccDropMovesSessions ------------------------------
\* Reversed witness: a friend taken out of the friends file has its sessions moved to no account and no instance (as
\* if its links had gone one by one), so a session's account is lost before the friend is added back. OutLeftAlone must
\* reject it.
EXTENDS MCAccounts

BrokenDropFriend(f) ==
    LET kn == known \ {f}
        nl == [links EXCEPT ![f] = << >>]
        nd == [dflt EXCEPT ![f] = None] IN
    IF f \in known /\ Consistent(acct, kn, nl, nd)
    THEN /\ known' = kn /\ links' = nl /\ dflt' = nd /\ insts' = InstsFor(kn)
         /\ ses' = SettledI(nl, nd, prov, InstsFor(kn), Friends)
         /\ told' = SettledToldI(nl, prov, InstsFor(kn), Friends) \cup {s \in Ids : ses[s].made /\ ses[s].friend = f}
         /\ Done("reload", f, None, 0, TRUE)
         \* The limit as a correct drop leaves it (BecomeIM): the friend out reads limited.
         /\ lim' = [g \in Friends |-> IF g \in kn THEN lim[g] ELSE TRUE]
         /\ UNCHANGED <<acct, prov, nextSes, bill, run>>
    ELSE Refused("reload", f, None, 0, {})

BrokenSpec == Init /\ [][Next \/ \E f \in Friends : BrokenDropFriend(f)]_vars
=============================================================================

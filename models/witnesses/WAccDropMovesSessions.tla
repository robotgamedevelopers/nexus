------------------------------ MODULE WAccDropMovesSessions ------------------------------
\* Reversed witness: an AI taken out of the AIs file has its sessions moved to no account and no instance (as
\* if its links had gone one by one), so a session's account is lost before the AI is added back. OutLeftAlone must
\* reject it.
EXTENDS MCAccounts

BrokenDropAI(f) ==
    LET kn == known \ {f}
        nl == [links EXCEPT ![f] = << >>]
        nd == [dflt EXCEPT ![f] = None] IN
    IF f \in known /\ Consistent(acct, kn, nl, nd)
    THEN /\ known' = kn /\ links' = nl /\ dflt' = nd /\ insts' = InstsFor(kn)
         /\ ses' = SettledI(nl, nd, prov, InstsFor(kn), AIs)
         /\ told' = SettledToldI(nl, prov, InstsFor(kn), AIs) \cup {s \in Ids : ses[s].made /\ ses[s].ai = f}
         /\ Done("reload", f, None, 0, TRUE)
         \* The limit as a correct drop leaves it (BecomeIM): the AI out reads limited.
         /\ lim' = [g \in AIs |-> IF g \in kn THEN lim[g] ELSE TRUE]
         /\ UNCHANGED <<acct, prov, nextSes, bill, run>>
    ELSE Refused("reload", f, None, 0, {})

BrokenSpec == Init /\ [][Next \/ \E f \in AIs : BrokenDropAI(f)]_vars
=============================================================================

------------------------------ MODULE WAccAddAIOpen ------------------------------
\* Reversed witness: an AI set up by an action is not limited. NewAILimited must reject it.
EXTENDS MCAccounts

BrokenAddAI(f) ==
    IF f \notin known
    THEN /\ known' = known \cup {f}
         /\ lim' = [lim EXCEPT ![f] = FALSE]
         /\ told' = {}
         /\ Done("ai", f, None, 0, TRUE)
         /\ UNCHANGED <<acct, prov, links, dflt, insts, ses, nextSes, bill, run>>
    ELSE Refused("ai", f, None, 0, {})

BrokenSpec == Init /\ [][NextLimitWith(LinkAccount, BrokenAddAI, Start, ReloadLimit, ReloadBilling)]_vars
=============================================================================

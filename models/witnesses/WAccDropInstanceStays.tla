------------------------------ MODULE WAccDropInstanceStays ------------------------------
\* Reversed witness: an instance dropped by an edit leaves its sessions on it. InstanceHeld must reject it.
EXTENDS MCAccounts

BrokenDropInstance(f, i) ==
    LET ni == [insts EXCEPT ![f] = Drop(@, i)] IN
    IF ConsistentI(acct, known, links, dflt, ni)
    THEN /\ insts' = ni
         /\ told' = {}
         /\ Done("reload", f, i, 0, TRUE)
         /\ UNCHANGED <<acct, prov, known, links, dflt, ses, nextSes>>
    ELSE Refused("reload", f, i, 0, {})

BrokenSpec == Init /\ [][NextWithI(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, Rotate, Create, ReloadDropLink, ReloadProvider, BrokenDropInstance)]_vars
=============================================================================

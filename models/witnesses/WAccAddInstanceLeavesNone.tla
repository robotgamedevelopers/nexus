------------------------------ MODULE WAccAddInstanceLeavesNone ------------------------------
\* Reversed witness: a friend's first instance added by an edit leaves its sessions on none, where the drivers file's
\* directory would still be used. InstanceHeld must reject it.
EXTENDS MCAccounts

BrokenAddInstance(f, i) ==
    LET ni == [insts EXCEPT ![f] = Append(@, i)] IN
    IF ConsistentI(acct, known, links, dflt, ni)
    THEN /\ insts' = ni
         /\ ses' = [s \in Ids |-> IF ses[s].made /\ ses[s].inst = None THEN ses[s]
                                  ELSE SettledI(links, dflt, prov, ni, known)[s]]
         /\ told' = {}
         /\ Done("reload", f, i, 0, TRUE)
         /\ UNCHANGED <<lim, bill, run, acct, prov, known, links, dflt, nextSes>>
    ELSE Refused("reload", f, i, 0, {})

BrokenSpec == Init /\ [][NextWithIM(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, Rotate, Create,
                                    ReloadDropLink, ReloadProvider, BrokenAddInstance, ReloadDropInstance,
                                    ReloadMoveInstance)]_vars
=============================================================================

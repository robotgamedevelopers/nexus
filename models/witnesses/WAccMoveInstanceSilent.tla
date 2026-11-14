------------------------------ MODULE WAccMoveInstanceSilent ------------------------------
\* Reversed witness: an instance moved in place (another machine or directory) tells none of its sessions, so their
\* drivers stay where they were unseen. MovedInstanceIsTold must reject it.
EXTENDS MCAccounts

BrokenMoveInstance(f, i) ==
    IF i \in Range(insts[f])
    THEN BecomeIM("moveinst", f, i, acct, prov, known, links, dflt, insts, {})
    ELSE Refused("moveinst", f, i, 0, {})

BrokenSpec == Init /\ [][NextWithIM(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, Rotate, Create,
                                    ReloadDropLink, ReloadProvider, ReloadAddInstance, ReloadDropInstance,
                                    BrokenMoveInstance)]_vars
=============================================================================

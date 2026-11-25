------------------------- MODULE WTermApplyStalls -------------------------
\* Broken ServerTerminals: a window's actions after its first wait for ever (an Applier that takes nothing once it has
\* applied one). Caught by ActionsApplied.
EXTENDS ServerTerminals

BrokenApply(c) == applied[c] = 0 /\ Apply(c)

BrokenSpec ==
    /\ Init /\ [][NextWith(BrokenApply, Detach, Sync)]_vars
    /\ \A c \in Clients : WF_vars(BrokenApply(c)) /\ WF_vars(Sync(c))

=============================================================================

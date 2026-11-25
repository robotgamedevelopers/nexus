------------------------- MODULE WTermRestartKept -------------------------
\* Broken ServerTerminals: a tab that holds a terminal of an earlier run keeps showing it as it was (the epoch never
\* compared, and no id of this run matches it). Caught by RestartNeverSilent.
EXTENDS ServerTerminals

BrokenSeenOf(c, s) ==
    IF seen[c][s] \in {"running", "ended"} /\ seenEp[c][s] # epoch THEN seen[c][s] ELSE SeenOf(c, s)

BrokenSync(c) == SyncWith(c, [s \in Slots |-> BrokenSeenOf(c, s)], [s \in Slots |-> SeenEpOf(c, s)])

BrokenSpec ==
    /\ Init /\ [][NextWith(Apply, Detach, BrokenSync)]_vars
    /\ \A c \in Clients : WF_vars(Apply(c)) /\ WF_vars(BrokenSync(c))

=============================================================================

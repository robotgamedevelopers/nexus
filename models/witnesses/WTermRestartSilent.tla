------------------------ MODULE WTermRestartSilent ------------------------
\* Broken ServerTerminals: a tab whose terminal is gone from the state is closed, whatever run it was of (the
\* epoch not compared): a restart drops the window's terminals silently. Caught by RestartShown.
EXTENDS ServerTerminals

BrokenSeenOf(c, s) ==
    IF seen[c][s] = "restarted" THEN "restarted"
    ELSE IF Live(s) /\ seenEp[c][s] = epoch THEN term[s].state
    ELSE IF Live(s) /\ seen[c][s] = "absent" THEN term[s].state
    ELSE "absent"

BrokenSync(c) == SyncWith(c, [s \in Slots |-> BrokenSeenOf(c, s)], [s \in Slots |-> SeenEpOf(c, s)])

BrokenSpec == Init /\ [][NextWith(Apply, Detach, BrokenSync)]_vars

=============================================================================

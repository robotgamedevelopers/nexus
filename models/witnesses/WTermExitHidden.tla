-------------------------- MODULE WTermExitHidden --------------------------
\* Broken ServerTerminals: the window shows an ended terminal as running (TerminalView::ended not read). Caught by
\* ExitShown.
EXTENDS ServerTerminals

BrokenSeenOf(c, s) ==
    IF SeenOf(c, s) = "ended" THEN "running" ELSE SeenOf(c, s)

BrokenSync(c) == SyncWith(c, [s \in Slots |-> BrokenSeenOf(c, s)], [s \in Slots |-> SeenEpOf(c, s)])

BrokenSpec ==
    /\ Init /\ [][NextWith(Apply, Detach, BrokenSync)]_vars
    /\ \A c \in Clients : WF_vars(Apply(c)) /\ WF_vars(BrokenSync(c))

=============================================================================

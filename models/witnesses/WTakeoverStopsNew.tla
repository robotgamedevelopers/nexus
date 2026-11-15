-------------------------- MODULE WTakeoverStopsNew --------------------------
\* Reversed witness: the installer stops any server running another build, as
\* a person did before takeover, cutting its open turn. StopOnlyOld must
\* reject it.
EXTENDS Takeover

BrokenAsk ==
    /\ server # None
    /\ server # placed
    /\ Gone
    /\ UNCHANGED <<placed, previous, places, service, handing, failed, rolled>>

BrokenNext == (Next /\ ~Ask) \/ BrokenAsk

BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================

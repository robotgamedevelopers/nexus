------------------------- MODULE WTakeoverStartsLast -------------------------
\* Reversed witness: what starts is any build, as a service whose program is
\* a copy that setup does not replace. StartsPlaced must reject it.
EXTENDS Takeover

BrokenStart ==
    /\ server = None
    /\ \E b \in AllBuilds : server' = b
    /\ UNCHANGED <<placed, previous, draining, late, turn, places, service, handing, failed, rolled>>

BrokenNext == (Next /\ ~Start) \/ BrokenStart

BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================

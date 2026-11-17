------------------------- MODULE WReconnectOnlyEnded -------------------------
\* Reversed witness: the window makes a session again only when its server
\* ends it (the shutdown a stopping server sends), never on silence. A server
\* that crashes tells nothing, and the window keeps its dead session forever.
\* Reconnects must reject it.
EXTENDS Reconnect

BrokenDial == DialWith(link = 0 \/ ended)

BrokenNext == (Next /\ ~Dial) \/ BrokenDial

BrokenSpec == Init /\ [][BrokenNext]_vars /\ WF_vars(BrokenDial) /\ WF_vars(Hear) /\ WF_vars(Time)
=============================================================================

---------------------------- MODULE WReconnectOnce ----------------------------
\* Reversed witness: the window runs its bootstrap only while it has never
\* held a session, as the link did before it reconnected by itself
\* (src/link/client.cpp before window/reconnect-after-takeover). After a
\* server stops it keeps the old server's session forever. Reconnects must
\* reject it.
EXTENDS Reconnect

BrokenDial == DialWith(link = 0)

BrokenNext == (Next /\ ~Dial) \/ BrokenDial

BrokenSpec == Init /\ [][BrokenNext]_vars /\ WF_vars(BrokenDial) /\ WF_vars(Hear) /\ WF_vars(Time)
=============================================================================

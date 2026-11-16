-------------------------- MODULE WReconnectCached --------------------------
\* Reversed witness: with no server running, the window makes its session
\* again from the CONNECT line it kept (the old server's port and key)
\* rather than failing: it talks to the old server's session after the
\* server stopped. DialsLive must reject it.
EXTENDS Reconnect

BrokenDial ==
    /\ Lost
    /\ due
    /\ due' = FALSE
    /\ IF srv # 0
       THEN /\ link' = srv
            /\ heard' = FALSE
            /\ quiet' = FALSE
            /\ ended' = FALSE
            /\ gen' = 1 - gen
       ELSE IF link # 0
            THEN /\ heard' = FALSE
                 /\ quiet' = FALSE
                 /\ ended' = FALSE
                 /\ gen' = 1 - gen
                 /\ UNCHANGED link
            ELSE UNCHANGED <<link, heard, quiet, ended, gen>>
    /\ UNCHANGED <<srv, started>>

BrokenNext == (Next /\ ~Dial) \/ BrokenDial

BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================

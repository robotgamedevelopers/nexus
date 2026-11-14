---------------------------- MODULE WSecDialogStays ----------------------------
\* Reversed witness: a window keeps its dialog open until the person closes it, even after the request was answered in
\* another window. DialogsClose must reject it.
EXTENDS MCSecrets

Keep(c, ids) == IF dialog[c].id # 0 THEN dialog[c] ELSE DialogAfter(c, ids)
KeepSync(c) == SyncWith(Received, Keep, c)

BrokenSpec == Init /\ [][NextWith(Ask, KeepSync, Receive, Timeout, Restart)]_vars
                   /\ \A r \in Ids : WF_vars(Timeout(r))
                   /\ \A c \in Clients : WF_vars(KeepSync(c))
                   /\ WF_vars(\E m \in up : Receive(m))
=============================================================================

-------------------------- MODULE WAddAISelects --------------------------
\* Reversed witness: add_ai makes the new tab the active one, as add_terminal does, so an AI arriving from the
\* server takes the person away from the tab they were on. AddAIExact must reject it.
EXTENDS MCWindow

BrokenAddAI(f) ==
    IF f.name /= "" /\ ~Standing(f.name)
    THEN LET at == AIEnd(order) IN
         /\ order' = [p \in 0 .. Count |-> IF p < at THEN order[p] ELSE IF p = at THEN nextId ELSE order[p - 1]]
         /\ active' = at
         /\ ais' = With(ais, nextId, f)
         /\ transcript' = With(transcript, nextId, <<>>)
         /\ draft' = With(draft, nextId, "")
         /\ queue' = With(queue, nextId, <<>>)
         /\ nextId' = nextId + 1
         /\ history' = Touch(history, nextId)
         /\ last' = DidAI("add_ai", 0, f, TRUE, nextId)
    ELSE /\ last' = DidAI("add_ai", 0, f, FALSE, 0)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextAIOps(BrokenAddAI, RemoveAI, MCArrivals)]_vars
=============================================================================

----------------------- MODULE WAddAIAmongTerminals -----------------------
\* Reversed witness: add_ai puts the AI's tab at the end of the strip, as add_terminal puts a terminal's, so
\* an AI stands behind a terminal. ZonesOrdered must reject it.
EXTENDS MCWindow

BrokenAddAI(f) ==
    IF f.name /= "" /\ ~Standing(f.name)
    THEN /\ order' = [p \in 0 .. Count |-> IF p < Count THEN order[p] ELSE nextId]
         /\ ais' = With(ais, nextId, f)
         /\ transcript' = With(transcript, nextId, <<>>)
         /\ draft' = With(draft, nextId, "")
         /\ queue' = With(queue, nextId, <<>>)
         /\ nextId' = nextId + 1
         /\ history' = IF Count = 0 THEN <<nextId>> ELSE history
         /\ last' = DidAI("add_ai", 0, f, TRUE, nextId)
         /\ UNCHANGED active
    ELSE /\ last' = DidAI("add_ai", 0, f, FALSE, 0)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextAIOps(BrokenAddAI, RemoveAI, MCArrivals)]_vars
=============================================================================

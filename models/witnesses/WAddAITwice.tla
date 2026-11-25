--------------------------- MODULE WAddAITwice ---------------------------
\* Reversed witness: add_ai takes a name an AI's tab already has, so the strip holds two tabs of one AI.
\* NamesOnce must reject it.
EXTENDS MCWindow

BrokenAddAI(f) ==
    IF f.name /= ""
    THEN LET at == AIEnd(order) IN
         /\ order' = [p \in 0 .. Count |-> IF p < at THEN order[p] ELSE IF p = at THEN nextId ELSE order[p - 1]]
         /\ active' = IF Count > 0 /\ active >= at THEN active + 1 ELSE active
         /\ ais' = With(ais, nextId, f)
         /\ transcript' = With(transcript, nextId, <<>>)
         /\ draft' = With(draft, nextId, "")
         /\ queue' = With(queue, nextId, <<>>)
         /\ nextId' = nextId + 1
         /\ history' = IF Count = 0 THEN <<nextId>> ELSE history
         /\ last' = DidAI("add_ai", 0, f, TRUE, nextId)
    ELSE /\ last' = DidAI("add_ai", 0, f, FALSE, 0)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextAIOps(BrokenAddAI, RemoveAI, MCArrivals)]_vars
=============================================================================

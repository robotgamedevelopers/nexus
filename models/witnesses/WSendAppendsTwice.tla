--------------------------- MODULE WSendAppendsTwice ---------------------------
\* Reversed witness: send appends the Person entry twice. Transcripts still only
\* grow and nothing else changes; SendAppendsOne must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF Sendable(i)
    THEN /\ LET e == [kind |-> "Person", text |-> draft[At(i)]] IN
            transcript' = [transcript EXCEPT ![At(i)] = Append(Append(@, e), e)]
         /\ draft' = [draft EXCEPT ![At(i)] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, queue>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, Queue, AppendEntry, MoveTab)]_vars
=============================================================================

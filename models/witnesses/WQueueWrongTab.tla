--------------------------- MODULE WQueueWrongTab ---------------------------
\* Reversed witness: queue appends the named tab's draft to the next tab's
\* queue (wrapping) instead of its own. SendTouchesNoOtherTab must reject it.
EXTENDS MCWindow

BrokenQueue(i) ==
    IF Sendable(i)
    THEN /\ queue' = [queue EXCEPT ![(i + 1) % N] = Append(@, draft[i])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, active, transcript>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, Send, BrokenQueue, AppendEntry)]_vars
=============================================================================

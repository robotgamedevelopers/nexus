-------------------------- MODULE WSendByPosition --------------------------
\* Reversed witness: send and queue take the position as the AI, as if the
\* tabs never moved, so after a move they address the AI whose tab stood
\* there first. SendTouchesNoOtherTab must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF i \in Tabs /\ i \in Ids /\ draft[i] \notin Blank
    THEN /\ transcript' = [transcript EXCEPT ![i] = Append(@, [kind |-> "Person", text |-> draft[i]])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, queue>>
    ELSE /\ last' = Did("send", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenQueue(i) ==
    IF i \in Tabs /\ i \in Ids /\ draft[i] \notin Blank
    THEN /\ queue' = [queue EXCEPT ![i] = Append(@, draft[i])]
         /\ draft' = [draft EXCEPT ![i] = ""]
         /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, transcript>>
    ELSE /\ last' = Did("queue", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, BrokenQueue, AppendEntry, MoveTab)]_vars
=============================================================================

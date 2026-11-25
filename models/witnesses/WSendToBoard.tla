----------------------------- MODULE WSendToBoard -----------------------------
\* Reversed witness: send on a dashboard's tab is taken, and the first AI's draft goes to that AI's transcript, as a
\* prompt box shown on a dashboard would send to whoever stands next. BoardTakesNoText must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Dashboard" /\ N > 0 /\ draft[0] \notin Blank
    THEN /\ transcript' = [transcript EXCEPT ![0] = Append(@, [kind |-> "Person", text |-> draft[0]])]
         /\ draft' = [draft EXCEPT ![0] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<ais, order, active, queue, nextId>>
    ELSE Send(i)

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, Queue, AppendEntry, MoveTab)]_vars
=============================================================================

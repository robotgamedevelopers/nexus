---------------------------- MODULE WSendToSprint ----------------------------
\* Reversed witness: send on the sprint tab is taken, and the first friend's
\* draft goes to that friend's transcript, as a prompt box shown on the sprint
\* tab would send to whoever stands next. SprintTakesNoText must reject it.
EXTENDS MCWindow

BrokenSend(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Sprint" /\ N > 0 /\ draft[0] \notin Blank
    THEN /\ transcript' = [transcript EXCEPT ![0] = Append(@, [kind |-> "Person", text |-> draft[0]])]
         /\ draft' = [draft EXCEPT ![0] = ""]
         /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, order, active, queue, nextId>>
    ELSE Send(i)

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, Queue, AppendEntry, MoveTab)]_vars
=============================================================================

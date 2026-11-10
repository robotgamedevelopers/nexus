--------------------------- MODULE WSendToTerminal ---------------------------
\* Reversed witness: send and queue addressed to a terminal's tab return true
\* (as if it took the text) and change nothing. TerminalTakesNoText must reject
\* it.
EXTENDS MCWindow

OnTerminal(i) == i \in Tabs /\ TabKind(At(i)) = "Terminal"

BrokenSend(i) ==
    IF OnTerminal(i)
    THEN /\ last' = Did("send", i, "", NoEntry, TRUE)
         /\ UNCHANGED model
    ELSE Send(i)

BrokenQueue(i) ==
    IF OnTerminal(i)
    THEN /\ last' = Did("queue", i, "", NoEntry, TRUE)
         /\ UNCHANGED model
    ELSE Queue(i)

BrokenSpec == Init /\ [][NextWith(SelectTab, SetDraft, BrokenSend, BrokenQueue, AppendEntry, MoveTab)]_vars
=============================================================================

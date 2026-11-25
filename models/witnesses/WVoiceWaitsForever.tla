------------------------------ MODULE WVoiceWaitsForever ------------------------------
\* Reversed witness: a stopped listen whose recognizer gives no final result waits for it forever. ListenEnds must reject it.
EXTENDS MCVoiceTurn

BrokenSpec == Init /\ [][Next]_vars /\ WF_vars(TickStop)
=============================================================================

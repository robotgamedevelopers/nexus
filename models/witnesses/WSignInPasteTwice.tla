--------------------------- MODULE WSignInPasteTwice ---------------------------
\* Reversed witness: the server forgets that a code went (it takes a code from the holder whenever the flow was asked,
\* or has had one), so a second code, resent or pasted again, reaches the process. OneCodePerFlow must reject it.
EXTENDS SignInPaste

TakesAgain(m) == flow[m.id].st \in Running /\ flow[m.id].opener = m.c /\ (flow[m.id].asked \/ flow[m.id].sent)

BrokenSpec == Init /\ [][ServerNextWith(TakesAgain) \/ WindowNextWith(LAMBDA c : TRUE)]_vars
=============================================================================

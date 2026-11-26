------------------------- MODULE WSignInPasteAnyWindow -------------------------
\* Reversed witness: the server passes on a code for a flow that waits for one from any window, not only the one
\* holding the flow (SignInDesk::code without its opener check). CodeOnlyFromHolder must reject it.
EXTENDS SignInPaste

TakesFromAny(m) == flow[m.id].st \in Running /\ flow[m.id].asked /\ ~flow[m.id].sent

BrokenSpec == Init /\ [][ServerNextWith(TakesFromAny) \/ WindowNextWith(LAMBDA c : TRUE)]_vars
=============================================================================

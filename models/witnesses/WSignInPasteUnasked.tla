-------------------------- MODULE WSignInPasteUnasked --------------------------
\* Reversed witness: the server takes a code from the holding window before its process asked for one (no flow
\* waiting for a code). CodeOnlyWhenAsked must reject it.
EXTENDS SignInPaste

TakesUnasked(m) == flow[m.id].st \in Running /\ flow[m.id].opener = m.c /\ ~flow[m.id].sent

BrokenSpec == Init /\ [][ServerNextWith(TakesUnasked) \/ WindowNextWith(LAMBDA c : TRUE)]_vars
=============================================================================

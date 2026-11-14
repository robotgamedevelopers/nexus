--------------------------- MODULE WSecNoClientOpens ---------------------------
\* Reversed witness: a request opens though no connected window can take it (none connected, or only older ones), and
\* its asker waits out the deadline for a dialog nobody sees. AskNeedsTaker must reject it.
EXTENDS MCSecrets

BrokenSpec == Init /\ [][NextWith(LAMBDA a, acct : AskWith(Busy, FALSE, a, acct), Sync, Receive, Timeout,
                                  Restart)]_vars
=============================================================================

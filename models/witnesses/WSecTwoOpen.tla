------------------------------ MODULE WSecTwoOpen ------------------------------
\* Reversed witness: a second request for an account with one open is opened too, so two dialogs ask for one secret
\* and either answer may be overwritten. OneOpenPerAccount must reject it.
EXTENDS MCSecrets

BrokenSpec == Init /\ [][NextWith(LAMBDA a, acct : AskWith(LAMBDA x : FALSE, NoTaker, a, acct), Sync, Receive,
                                  Timeout, Restart)]_vars
=============================================================================

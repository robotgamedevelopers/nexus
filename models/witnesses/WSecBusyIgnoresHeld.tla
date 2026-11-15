---------------------------- MODULE WSecBusyIgnoresHeld ----------------------------
\* Reversed witness: an ask for an account whose value is held (its write being retried) is not refused busy; only an
\* open request makes it busy. A second request opens beside the held one, and two values for one account are in
\* flight. OneOpenPerAccount must reject it, on an instance with two requests and a failing disk.
EXTENDS MCSecrets

BusyOpenOnly(acct) == \E q \in OpenIds : req[q].acct = acct

BrokenSpec == Init /\ [][NextWith(LAMBDA a, acct : AskWith(BusyOpenOnly, NoTaker, a, acct), Sync, Receive,
                                  Timeout, Restart) \/ RetryNext \/ DiskNext]_vars
=============================================================================

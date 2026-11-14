-------------------------- MODULE WSecRelayToWindows --------------------------
\* Reversed witness: the state every window receives says which accounts have a secret by carrying it, so another
\* window could show it filled in. NoSecretToWindows must reject it.
EXTENDS MCSecrets

LeakyReceived(c) ==
    Pub \cup {[id |-> 0, acct |-> a, asker |-> file[a]] : a \in {x \in Accounts : file[x] # NoValue}}

BrokenSpec == Init /\ [][NextWith(Ask, LAMBDA c : SyncWith(LeakyReceived, DialogAfter, c), Receive, Timeout,
                                  Restart)]_vars
=============================================================================

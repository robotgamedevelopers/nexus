------------------------------ MODULE WAccProviderKeepsHarness ------------------------------
\* Reversed witness: a reload that changes an account's provider leaves its sessions on the old harness. HarnessMatches must reject it.
EXTENDS MCAccounts

BrokenProvider(a, p) ==
    /\ prov' = [prov EXCEPT ![a] = p]
    /\ Done("reload", None, a, 0, TRUE) /\ told' = {}
    /\ UNCHANGED <<acct, known, links, dflt, ses, nextSes>>

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, Rotate, Create, ReloadDropLink, BrokenProvider)]_vars
=============================================================================

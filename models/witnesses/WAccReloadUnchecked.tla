------------------------------ MODULE WAccReloadUnchecked ------------------------------
\* Reversed witness: a reload applies a hand edit without checking it, so a dropped link may stay the default. DefaultLinked must reject it.
EXTENDS MCAccounts

BrokenDropLink(f, a) == Become("reload", f, a, acct, prov, known, [links EXCEPT ![f] = Drop(@, a)], dflt)

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, Rotate, Create, BrokenDropLink, ReloadProvider)]_vars
=============================================================================

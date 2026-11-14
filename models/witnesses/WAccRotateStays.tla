------------------------------ MODULE WAccRotateStays ------------------------------
\* Reversed witness: rotation never switches: it only tells the session. RotateMoves must reject it.
EXTENDS MCAccounts

BrokenRotate(s) == Refused("rotate", None, None, s, IF ses[s].made THEN {s} ELSE {})

BrokenSpec == Init /\ [][NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, BrokenRotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

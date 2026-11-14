------------------------------ MODULE WAccAddResets ------------------------------
\* Reversed witness: adding an account already set up is refused, but signs it out. RefusedChangesNothing must reject it.
EXTENDS MCAccounts

BrokenAdd(a) ==
    IF acct[a] = "absent"
    THEN AddAccount(a)
    ELSE /\ acct' = [acct EXCEPT ![a] = "out"]
         /\ Done("add", None, a, 0, FALSE) /\ told' = {}
         /\ UNCHANGED <<insts, prov, known, links, dflt, ses, nextSes>>

BrokenSpec == Init /\ [][NextWith(BrokenAdd, RemoveAccount, Unlink, SetDefault, SetSession, Rotate, Create, ReloadDropLink, ReloadProvider)]_vars
=============================================================================

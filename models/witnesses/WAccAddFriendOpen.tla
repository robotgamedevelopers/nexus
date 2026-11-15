------------------------------ MODULE WAccAddFriendOpen ------------------------------
\* Reversed witness: a friend set up by an action is not limited. NewFriendLimited must reject it.
EXTENDS MCAccounts

BrokenAddFriend(f) ==
    IF f \notin known
    THEN /\ known' = known \cup {f}
         /\ lim' = [lim EXCEPT ![f] = FALSE]
         /\ told' = {}
         /\ Done("friend", f, None, 0, TRUE)
         /\ UNCHANGED <<acct, prov, links, dflt, insts, ses, nextSes, bill, run>>
    ELSE Refused("friend", f, None, 0, {})

BrokenSpec == Init /\ [][NextLimitWith(LinkAccount, BrokenAddFriend, Start, ReloadLimit, ReloadBilling)]_vars
=============================================================================

----------------------- MODULE WAddFriendAmongTerminals -----------------------
\* Reversed witness: add_friend puts the friend's tab at the end of the strip, as add_terminal puts a terminal's, so
\* a friend stands behind a terminal. ZonesOrdered must reject it.
EXTENDS MCWindow

BrokenAddFriend(f) ==
    IF f.name /= "" /\ ~Standing(f.name)
    THEN /\ order' = [p \in 0 .. Count |-> IF p < Count THEN order[p] ELSE nextId]
         /\ friends' = With(friends, nextId, f)
         /\ transcript' = With(transcript, nextId, <<>>)
         /\ draft' = With(draft, nextId, "")
         /\ queue' = With(queue, nextId, <<>>)
         /\ nextId' = nextId + 1
         /\ history' = IF Count = 0 THEN <<nextId>> ELSE history
         /\ last' = DidFriend("add_friend", 0, f, TRUE, nextId)
         /\ UNCHANGED active
    ELSE /\ last' = DidFriend("add_friend", 0, f, FALSE, 0)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextFriendOps(BrokenAddFriend, RemoveFriend, MCArrivals)]_vars
=============================================================================

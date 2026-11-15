-------------------------- MODULE WAddFriendSelects --------------------------
\* Reversed witness: add_friend makes the new tab the active one, as add_terminal does, so a friend arriving from the
\* server takes the person away from the tab they were on. AddFriendExact must reject it.
EXTENDS MCWindow

BrokenAddFriend(f) ==
    IF f.name /= "" /\ ~Standing(f.name)
    THEN LET at == FriendEnd(order) IN
         /\ order' = [p \in 0 .. Count |-> IF p < at THEN order[p] ELSE IF p = at THEN nextId ELSE order[p - 1]]
         /\ active' = at
         /\ friends' = With(friends, nextId, f)
         /\ transcript' = With(transcript, nextId, <<>>)
         /\ draft' = With(draft, nextId, "")
         /\ queue' = With(queue, nextId, <<>>)
         /\ nextId' = nextId + 1
         /\ history' = Touch(history, nextId)
         /\ last' = DidFriend("add_friend", 0, f, TRUE, nextId)
    ELSE /\ last' = DidFriend("add_friend", 0, f, FALSE, 0)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextFriendOps(BrokenAddFriend, RemoveFriend, MCArrivals)]_vars
=============================================================================

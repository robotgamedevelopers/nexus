--------------------------- MODULE WAddFriendTwice ---------------------------
\* Reversed witness: add_friend takes a name a friend's tab already has, so the strip holds two tabs of one friend.
\* NamesOnce must reject it.
EXTENDS MCWindow

BrokenAddFriend(f) ==
    IF f.name /= ""
    THEN LET at == FriendEnd(order) IN
         /\ order' = [p \in 0 .. Count |-> IF p < at THEN order[p] ELSE IF p = at THEN nextId ELSE order[p - 1]]
         /\ active' = IF Count > 0 /\ active >= at THEN active + 1 ELSE active
         /\ friends' = With(friends, nextId, f)
         /\ transcript' = With(transcript, nextId, <<>>)
         /\ draft' = With(draft, nextId, "")
         /\ queue' = With(queue, nextId, <<>>)
         /\ nextId' = nextId + 1
         /\ history' = IF Count = 0 THEN <<nextId>> ELSE history
         /\ last' = DidFriend("add_friend", 0, f, TRUE, nextId)
    ELSE /\ last' = DidFriend("add_friend", 0, f, FALSE, 0)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextFriendOps(BrokenAddFriend, RemoveFriend, MCArrivals)]_vars
=============================================================================

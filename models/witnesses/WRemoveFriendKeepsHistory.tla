---------------------- MODULE WRemoveFriendKeepsHistory ----------------------
\* Reversed witness: removing a friend's tab leaves its id in the selection history, so the history names a tab that
\* no longer stands (trace_walk's --faulty-remove-friend is this fault in code). HistoryOpen must reject it.
EXTENDS MCWindow

BrokenRemoveFriend(i) ==
    IF FriendTab(i)
    THEN LET f == At(i) IN
         /\ order' = Removed(order, i)
         /\ UNCHANGED history
         /\ active' = ClosedActive(active, i, order, Forget(history, f))
         /\ friends' = Keep(friends, Ids \ {f})
         /\ transcript' = Keep(transcript, Ids \ {f})
         /\ draft' = Keep(draft, Ids \ {f})
         /\ queue' = Keep(queue, Ids \ {f})
         /\ last' = DidFriend("remove_friend", i, friends[f], TRUE, f)
         /\ UNCHANGED nextId
    ELSE /\ last' = DidFriend("remove_friend", i, NoFriend, FALSE, 0)
         /\ UNCHANGED model

BrokenSpec == Init /\ [][NextFriendOps(AddFriend, BrokenRemoveFriend, MCArrivals)]_vars
=============================================================================

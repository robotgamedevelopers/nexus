----------------------- MODULE WRemoveFriendActiveWrong -----------------------
\* Reversed witness: removing the active friend's tab makes the tab before it active, not the one selected before it.
\* RemoveActiveRule must reject it.
EXTENDS MCWindow

BrokenIndex(a, i, h) ==
    IF a = i /\ i > 0 THEN i - 1 ELSE ClosedActive(a, i, order, h)

BrokenRemoveFriend(i) ==
    IF FriendTab(i)
    THEN LET f == At(i) IN
         /\ order' = Removed(order, i)
         /\ history' = ClosedHistory(active, i, order, Forget(history, f))
         /\ active' = BrokenIndex(active, i, Forget(history, f))
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

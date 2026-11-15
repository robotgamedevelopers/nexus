----------------------- MODULE WCloseLeavesHistoryEmpty -----------------------
\* Reversed witness: close_tab only forgets the closed tab's id, as it did before friends could arrive without being
\* selected. In a strip that started empty, a terminal opened and a friend arrived; closing the terminal leaves the
\* friend's tab showing with the history empty (the counterexample TLC found in MCWindowFriendsEmpty.cfg). Window.tla's
\* ClosedHistory enters the tab that shows. HistoryEndsActive must reject it.
EXTENDS MCWindow

BrokenClose(i) ==
    IF i \in Tabs /\ TabKind(At(i)) = "Terminal"
    THEN /\ order' = Removed(order, i)
         /\ history' = Forget(history, At(i))
         /\ active' = ClosedActive(active, i, order, Forget(history, At(i)))
         /\ last' = Did("close", i, "", NoEntry, TRUE)
         /\ UNCHANGED <<friends, transcript, draft, queue, nextId>>
    ELSE /\ last' = Did("close", i, "", NoEntry, FALSE)
         /\ UNCHANGED model

BrokenNext ==
    \/ NextWithAll(SelectTab, SetDraft, Send, Queue, AppendEntry, MoveTab, AddTerminal, BrokenClose)
    \/ \E i \in Idx : TakeQueued(i)
    \/ \E f \in MCArrivals : AddFriend(f)
    \/ \E i \in Idx : RemoveFriend(i)

BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================

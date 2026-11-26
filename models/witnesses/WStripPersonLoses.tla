--------------------------- MODULE WStripPersonLoses ---------------------------
\* Reversed witness: the person's choice of a tab leaves the saved tab
\* waiting, so the AIs' arrival takes the tab they chose away from them.
\* PersonWins must reject it.
EXTENDS MCAIStrip

BrokenSelect(n) ==
    /\ n \in Ran(strip) \cup {NoAI}
    /\ selected' = n
    /\ act' = "select"
    /\ UNCHANGED <<strip, drafts, kept, saved, pending, latest, listed, haveList, exact, file, others>>

BrokenNext ==
    \/ \E L \in Lists, ex \in BOOLEAN : List(L, ex)
    \/ Same
    \/ \E n \in Names, t \in Texts : Type(n, t)
    \/ \E n \in Names \cup {NoAI} : BrokenSelect(n)
    \/ Restart
    \/ \E n \in Names, t \in OtherTexts : OtherKeep(n, t)
    \/ \E n \in Names : OtherTake(n)

BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================

----------------------------- MODULE PluginTabs -----------------------------
EXTENDS Naturals, Sequences, FiniteSets, TLC

Ids == {1, 2, 3}
Home == [i \in Ids |-> IF i = 2 THEN "tools" ELSE "writing"]
Rank(g) == IF g = "writing" THEN 1 ELSE 2
VARIABLES tabs, groups, transfers
vars == <<tabs, groups, transfers>>
Init == tabs = <<>> /\ groups = Home /\ transfers = FALSE
Members(s) == {s[i] : i \in DOMAIN s}
Insert(s, pos, id) == SubSeq(s, 1, pos - 1) \o <<id>> \o SubSeq(s, pos, Len(s))
Drop(s, pos) == SubSeq(s, 1, pos - 1) \o SubSeq(s, pos + 1, Len(s))
Open(id) ==
    LET n == Cardinality({i \in DOMAIN tabs : Rank(groups[tabs[i]]) <= Rank(Home[id])})
    IN /\ id \notin Members(tabs)
       /\ tabs' = Insert(tabs, n + 1, id)
       /\ UNCHANGED <<groups, transfers>>
Close(a) == /\ tabs' = Drop(tabs, a)
            /\ groups' = [groups EXCEPT ![tabs[a]] = Home[tabs[a]]]
            /\ UNCHANGED transfers
Permits(from, to) == from = to \/ (transfers /\ from = "tools" /\ to = "writing")
Move(a, b) ==
    /\ Permits(groups[tabs[a]], groups[tabs[b]])
    /\ groups' = [groups EXCEPT ![tabs[a]] = groups[tabs[b]]]
    /\ tabs' = Insert(Drop(tabs, a), b, tabs[a])
    /\ UNCHANGED transfers
OrderByGroup(s, g) == SelectSeq(s, LAMBDA id : g[id] = "writing") \o SelectSeq(s, LAMBDA id : g[id] = "tools")
Reload(enabled) ==
    /\ transfers' = enabled
    /\ groups' = IF enabled THEN groups ELSE Home
    /\ tabs' = OrderByGroup(tabs, groups')
Next ==
    \/ \E id \in Ids : Open(id)
    \/ \E a \in DOMAIN tabs : Close(a)
    \/ \E a, b \in DOMAIN tabs : Move(a, b)
    \/ \E enabled \in BOOLEAN : Reload(enabled)
Spec == Init /\ [][Next]_vars
\* Ignore group admission and rotate tabs without reconciling their membership.
Unfenced == \E a, b \in DOMAIN tabs : /\ tabs' = Insert(Drop(tabs, a), b, tabs[a])
                                        /\ UNCHANGED <<groups, transfers>>
Broken == Init /\ [][Next \/ Unfenced]_vars
Unique == Cardinality(Members(tabs)) = Len(tabs)
Grouped == tabs = OrderByGroup(tabs, groups)
Authorized == \A id \in Ids : Permits(Home[id], groups[id])
=============================================================================

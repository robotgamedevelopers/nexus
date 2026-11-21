----------------------------- MODULE PluginTabs -----------------------------
EXTENDS Naturals, Sequences, FiniteSets, TLC

\* Two instances of one registered group and one of another. Identity is kept through a move.
Ids == {1, 2, 3}
Group(i) == IF i = 2 THEN "tools" ELSE "writing"
VARIABLE tabs
Init == tabs = <<>>
Members(s) == {s[i] : i \in DOMAIN s}
Insert(s, pos, id) == SubSeq(s, 1, pos - 1) \o <<id>> \o SubSeq(s, pos, Len(s))
Drop(s, pos) == SubSeq(s, 1, pos - 1) \o SubSeq(s, pos + 1, Len(s))
Open(id) ==
    LET group == {i \in DOMAIN tabs : Group(tabs[i]) = Group(id)}
        last == IF group = {} THEN Len(tabs) ELSE CHOOSE k \in group : \A j \in group : j <= k
    IN /\ id \notin Members(tabs)
       /\ tabs' = Insert(tabs, last + 1, id)
Move(a, b) == tabs' = Insert(Drop(tabs, a), b, tabs[a])
Next(check) ==
    \/ \E id \in Ids : Open(id)
    \/ \E a \in DOMAIN tabs : tabs' = Drop(tabs, a)
    \/ \E a, b \in DOMAIN tabs : /\ (~check \/ Group(tabs[a]) = Group(tabs[b]))
                                  /\ Move(a, b)
Spec == Init /\ [][Next(TRUE)]_tabs
Broken == Init /\ [][Next(FALSE)]_tabs
Unique == Cardinality(Members(tabs)) = Len(tabs)
Grouped == \A a, b, c \in DOMAIN tabs : a < b /\ b < c /\ Group(tabs[a]) = Group(tabs[c])
                                          => Group(tabs[b]) = Group(tabs[a])
=============================================================================

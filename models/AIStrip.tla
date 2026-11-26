---------------------------- MODULE AIStrip ----------------------------
(***************************************************************************)
(* The AI tabs as the window keeps them from the server's state        *)
(* (src/sync/ai_strip.h AIStrip, over sync/client.h applyAIs   *)
(* and Model::add_ais, remove_ais): the AIs' part of the strip *)
(* in order, each tab's draft, the drafts kept for AIs without a tab,  *)
(* the AI whose tab shows, and the tab saved as selected at the last   *)
(* quit, which waits for its AI.                                       *)
(*                                                                         *)
(* The outside events: a state's list of AIs arrives, exact (the       *)
(* server's own list) or not (an older server's sessions, which only add); *)
(* the same state arrives again; the person types a draft or chooses a     *)
(* tab; the window quits and starts again. A name the server sends that is *)
(* not an AI's name (Bad) never gets a tab.                             *)
(*                                                                         *)
(* `latest` is a ghost: what the person last typed to each AI. The     *)
(* rule the strip keeps (DraftsKept) is that it is never lost while the    *)
(* window runs: in the AI's tab, or kept by name when the tab goes,    *)
(* and back in the AI's next tab. A quit keeps the kept drafts (saved, *)
(* tab_order.h) and not the drafts of the tabs open at the quit, which the *)
(* window has never saved: Restart says so in `latest`.                    *)
(*                                                                         *)
(* The kept drafts' file is shared by every window of the same server      *)
(* (tab_order.h KeptDraftsStore): `file` is that server's part of it. Each   *)
(* change the window makes is merged into the file as it is then, under    *)
(* the file's lock (applyKeptChange): a draft kept for a name the file     *)
(* already holds another draft for is joined to it after Sep, and a draft  *)
(* given back leaves the file only if the file still holds exactly it. The *)
(* other windows are the environment: OtherKeep and OtherTake. A start     *)
(* reads its kept drafts from the file. `others` is a ghost: the drafts the  *)
(* other windows kept that no window has taken back. The rule the file     *)
(* keeps (FileKeepsOthers) is that this window's changes never lose them.  *)
(* A window's change is one step here: its writer thread makes the         *)
(* window's changes in order, each under the lock, and the window reads    *)
(* the file only at its start.                                             *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Names,          \* the AIs' names a list may hold, each an AI's name by the server's rule
    Bad,            \* names a list may hold that are not an AI's name
    Texts,          \* the drafts the person may type, "" among them
    SavedOrder,     \* the AIs' order saved at the last quit: a sequence of distinct names
    MaxList,        \* the longest list a state carries
    OtherTexts      \* the drafts other windows of the server may keep, none of them ""

NoAI == ""      \* the tab that shows is no AI's (the sprint tab); "" is never an AI's name

Sep == "\n\n"       \* between two drafts the file joins for one name (tab_order.h kKeptJoin)

ASSUME NoAI \notin Names /\ "" \in Texts /\ "" \notin OtherTexts

VARIABLES
    strip,      \* the AI tabs, in order: a sequence of distinct names
    drafts,     \* each tab's draft: a function on the names in strip
    kept,       \* the drafts kept for AIs without a tab: a function on some names, never to ""
    selected,   \* the AI whose tab shows, or NoAI
    saved,      \* the AI whose tab was saved as selected at the last quit, or NoAI
    pending,    \* the saved tab still waits for its AI
    latest,     \* ghost: what the person last typed to each AI
    listed,     \* ghost: the AIs' names of the last list taken
    haveList,   \* ghost: a list has been taken since the start
    exact,      \* ghost: that list was the server's own
    act,        \* ghost: the last event
    file,       \* the server's kept drafts in the shared file: a function on some names, never to ""
    others      \* ghost: the drafts other windows kept and no window took back, as <<name, draft>>

vars == <<strip, drafts, kept, selected, saved, pending, latest, listed, haveList, exact, act, file, others>>

Ran(s) == {s[i] : i \in DOMAIN s}
Pos(s, n) == CHOOSE i \in DOMAIN s : s[i] = n
Distinct(s) == \A i, j \in DOMAIN s : i # j => s[i] # s[j]

\* Text t is in text s.
Has(s, t) == \E i \in 0..(Len(s) - Len(t)) : SubSeq(s, i + 1, i + Len(t)) = t

\* The file with draft t kept for n, merged into it (applyKeptChange's put).
Put(f, n, t) == [m \in DOMAIN f \cup {n} |-> IF m = n /\ n \in DOMAIN f /\ f[n] # t THEN f[n] \o Sep \o t
                                            ELSE IF m = n THEN t ELSE f[m]]
\* The file with draft t, given back to n, taken from it if it still holds exactly t (applyKeptChange's take).
Take(f, n, t) == IF n \in DOMAIN f /\ f[n] = t THEN [m \in DOMAIN f \ {n} |-> f[m]] ELSE f

\* The lists a state may carry: distinct names, good and bad, up to MaxList of them.
Lists == {s \in UNION {[1..k -> Names \cup Bad] : k \in 0..MaxList} : Distinct(s)}

\* What the window shows of a list: its AIs' names, in order, the bad ones left out (aisOf).
Valid(L) == SelectSeq(L, LAMBDA n : n \in Names)

\* What the window holds for AI n: its tab's draft, or the draft kept for it, or nothing.
Shown(n) == IF n \in Ran(strip) THEN drafts[n] ELSE IF n \in DOMAIN kept THEN kept[n] ELSE ""

Init ==
    /\ strip = <<>>
    /\ drafts = [n \in {} |-> ""]
    /\ kept = [n \in {} |-> ""]
    /\ selected = NoAI
    /\ saved \in Names \cup {NoAI}
    /\ pending = (saved # NoAI)
    /\ latest = [n \in Names |-> ""]
    /\ listed = {}
    /\ haveList = FALSE
    /\ exact = FALSE
    /\ act = "start"
    /\ file = [n \in {} |-> ""]
    /\ others = {}

\* A state's list arrives (AIStrip::take, applyAIs). With `ex`, each tab whose AI the list does not name
\* goes, its draft kept by name when it has one; the tabs that stay keep their order. Each AI the list names that
\* has no tab gets one at the end, those SavedOrder names first in its order, then the rest in the list's, with the
\* draft kept for its name, which then leaves `kept`. The tab that shows stays, unless it went: then one of the tabs
\* left, or none of an AI's, as the strip's history says (Model::remove_ais). While the saved tab waits, its
\* AI's tab, once there, is selected; a list from the server that does not name it ends the wait.
\*
\* ListWith takes the rule's parts as switches, every one on in List; each reversed witness (witnesses/WStrip*.tla)
\* turns one off, as a strip that lacked that part would behave.
Reverse(s) == [i \in 1..Len(s) |-> s[Len(s) - i + 1]]

Right == [keep |-> TRUE, valid |-> TRUE, remove |-> TRUE, order |-> TRUE, saved |-> TRUE, restore |-> TRUE,
          erase |-> TRUE, merge |-> TRUE]

\* The window's change of the kept drafts made in the file: each draft of `keep` put, each of `back` (given back
\* from `k`) taken. Without F.merge, the window's whole map written over the file, as a window that did not read
\* the file under its lock would.
RECURSIVE PutAll(_, _, _), TakeAll(_, _, _)
PutAll(f, S, d) == IF S = {} THEN f ELSE LET n == CHOOSE m \in S : TRUE IN PutAll(Put(f, n, d[n]), S \ {n}, d)
TakeAll(f, S, k) == IF S = {} THEN f ELSE LET n == CHOOSE m \in S : TRUE IN TakeAll(Take(f, n, k[n]), S \ {n}, k)

ListWith(L, ex, F) ==
    LET shown == IF F.valid THEN Valid(L) ELSE L
        V == Ran(shown)
        gone == IF ex /\ F.remove THEN Ran(strip) \ V ELSE {}
        stay == SelectSeq(strip, LAMBDA n : n \notin gone)
        survivors == IF F.order THEN stay ELSE Reverse(stay)
        keep == IF F.keep THEN {n \in gone : drafts[n] # ""} ELSE {}
        kept1 == [n \in DOMAIN kept \cup keep |-> IF n \in keep THEN drafts[n] ELSE kept[n]]
        fresh == V \ Ran(survivors)
        added == IF F.saved
                 THEN SelectSeq(SavedOrder, LAMBDA n : n \in fresh) \o
                      SelectSeq(shown, LAMBDA n : n \in fresh /\ n \notin Ran(SavedOrder))
                 ELSE SelectSeq(shown, LAMBDA n : n \in fresh)
        strip1 == survivors \o added
        after == IF selected \in gone THEN Ran(survivors) \cup {NoAI} ELSE {selected}
        back == fresh \cap DOMAIN kept1
        kept2 == IF F.erase THEN [n \in DOMAIN kept1 \ fresh |-> kept1[n]] ELSE kept1
        file1 == PutAll(TakeAll(file, back, kept1), keep, drafts)
    IN
    /\ strip' = strip1
    /\ drafts' = [n \in Ran(strip1) |-> IF n \in Ran(survivors) THEN drafts[n]
                                        ELSE IF n \in DOMAIN kept1 THEN kept1[n] ELSE ""]
    /\ kept' = kept2
    /\ IF keep = {} /\ back = {} THEN file' = file
       ELSE file' = IF F.merge THEN file1 ELSE kept2
    \* a draft given back from the file is shown to the person: no longer the file's to keep
    /\ others' = {p \in others : p[1] \in DOMAIN file' \/ p[1] \notin back}
    /\ IF F.restore /\ pending /\ saved \in Ran(strip1)
       THEN /\ selected' = saved
            /\ pending' = FALSE
       ELSE /\ selected' \in after
            /\ pending' = (pending /\ ~ex)
    /\ listed' = V
    /\ haveList' = TRUE
    /\ exact' = ex
    /\ act' = "list"
    /\ UNCHANGED <<saved, latest>>

List(L, ex) == ListWith(L, ex, Right)

\* The same state again (a copy of it, or a state whose AIs did not change): nothing.
Same ==
    /\ act' = "same"
    /\ UNCHANGED <<strip, drafts, kept, selected, saved, pending, latest, listed, haveList, exact, file, others>>

\* The person types `t` into n's tab.
Type(n, t) ==
    /\ n \in Ran(strip)
    /\ drafts' = [drafts EXCEPT ![n] = t]
    /\ latest' = [latest EXCEPT ![n] = t]
    /\ act' = "type"
    /\ UNCHANGED <<strip, kept, selected, saved, pending, listed, haveList, exact, file, others>>

\* The person chooses a tab (n's, or the sprint tab): the saved tab no longer waits.
Select(n) ==
    /\ n \in Ran(strip) \cup {NoAI}
    /\ selected' = n
    /\ pending' = FALSE
    /\ act' = "select"
    /\ UNCHANGED <<strip, drafts, kept, saved, latest, listed, haveList, exact, file, others>>

\* The window quits and starts again: the kept drafts read back from the file (tab_order.h loadKeptDrafts), the tab
\* that showed saved and waited for (but while the saved tab waits, the selection is not saved: the one saved before
\* stays), no tab of an AI's until a state arrives. The drafts of the tabs open at the quit are not saved.
Restart ==
    LET s == IF pending THEN saved ELSE selected IN
    /\ strip' = <<>>
    /\ drafts' = [n \in {} |-> ""]
    /\ saved' = s
    /\ pending' = (s # NoAI)
    /\ selected' = NoAI
    /\ kept' = file
    /\ latest' = [n \in Names |-> IF n \in DOMAIN file THEN file[n] ELSE ""]
    /\ listed' = {}
    /\ haveList' = FALSE
    /\ exact' = FALSE
    /\ act' = "restart"
    /\ UNCHANGED <<file, others>>

\* Another window of the server keeps draft t for n: merged into the file under its lock. Each of its drafts once.
OtherKeep(n, t) ==
    /\ <<n, t>> \notin others
    /\ file' = Put(file, n, t)
    /\ others' = others \cup {<<n, t>>}
    /\ act' = "other-keep"
    /\ UNCHANGED <<strip, drafts, kept, selected, saved, pending, latest, listed, haveList, exact>>

\* Another window, started since, gives back what the file holds for n: the drafts are its tab's now. With no
\* OtherTexts the window is alone on its server, and there is no other window.
OtherTake(n) ==
    /\ OtherTexts # {}
    /\ n \in DOMAIN file
    /\ file' = [m \in DOMAIN file \ {n} |-> file[m]]
    /\ others' = {p \in others : p[1] # n}
    /\ act' = "other-take"
    /\ UNCHANGED <<strip, drafts, kept, selected, saved, pending, latest, listed, haveList, exact>>

NextWith(F) ==
    \/ \E L \in Lists, ex \in BOOLEAN : ListWith(L, ex, F)
    \/ Same
    \/ \E n \in Names, t \in Texts : Type(n, t)
    \/ \E n \in Names \cup {NoAI} : Select(n)
    \/ Restart
    \/ \E n \in Names, t \in OtherTexts : OtherKeep(n, t)
    \/ \E n \in Names : OtherTake(n)

Next == NextWith(Right)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* Properties.

TypeOK ==
    /\ strip \in Seq(Names \cup Bad)
    /\ DOMAIN drafts = Ran(strip)
    /\ \A n \in DOMAIN kept : n \in Names /\ kept[n] \in STRING
    /\ \A n \in DOMAIN file : n \in Names /\ file[n] \in STRING /\ file[n] # ""
    /\ others \subseteq Names \X OtherTexts
    /\ selected \in Names \cup {NoAI}
    /\ pending \in BOOLEAN

\* No name that is not an AI's ever has a tab, and no AI has two.
NamesValid == Ran(strip) \subseteq Names /\ Distinct(strip)

\* Nothing the person typed is lost while the window runs: what they last typed to an AI is in its tab, or kept.
DraftsKept == \A n \in Names : latest[n] = Shown(n)

\* A draft is kept only for an AI without a tab, and only when it holds something.
KeptOnlyAbsent == \A n \in DOMAIN kept : n \notin Ran(strip) /\ kept[n] # ""

\* No change of this window's loses a draft another window kept: it stays in the file until a window takes it back.
FileKeepsOthers == \A p \in others : p[1] \in DOMAIN file /\ Has(file[p[1]], p[2])

\* After an exact list, the strip is that list's AIs; after any list, it holds them all.
StripExact == haveList /\ exact => Ran(strip) = listed
StripCovers == haveList => listed \subseteq Ran(strip)

\* The saved tab waits only while its AI has no tab.
WaitsOnlyForAbsent == pending => saved \notin Ran(strip)

\* The tabs that stay keep their order.
SurvivorsKeepOrder ==
    [][\A a, b \in Ran(strip) \cap Ran(strip') :
          Pos(strip, a) < Pos(strip, b) => Pos(strip', a) < Pos(strip', b)]_vars

\* A list's new tabs come after every tab that stayed, those the saved order names first, in its order.
NewInSavedOrder ==
    [][act' = "list" =>
        LET new == Ran(strip') \ Ran(strip) IN
        /\ \A a \in new, b \in Ran(strip') \ new : Pos(strip', b) < Pos(strip', a)
        /\ \A a, b \in new \cap Ran(SavedOrder) :
               Pos(SavedOrder, a) < Pos(SavedOrder, b) => Pos(strip', a) < Pos(strip', b)
        /\ \A a \in new \cap Ran(SavedOrder), b \in new \ Ran(SavedOrder) : Pos(strip', a) < Pos(strip', b)]_vars

\* While the saved tab waits, a list that brings its AI's tab selects it.
RestoreSaved ==
    [][(act' = "list" /\ pending /\ saved \in Ran(strip')) => (selected' = saved /\ ~pending')]_vars

\* The person's choice wins: after it nothing waits, and the wait never starts again but at a start.
PersonWins ==
    [][/\ act' = "select" => ~pending'
       /\ pending' => (pending \/ act' = "restart")]_vars

=============================================================================

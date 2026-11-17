-------------------------- MODULE FriendStripTrace --------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real                 *)
(* nexus::sync::FriendStrip, over a real Model, is a behaviour of          *)
(* FriendStrip.tla.                                                        *)
(*                                                                         *)
(* tests/model/strip_walk.cpp drives the code with a seeded random walk    *)
(* and writes FriendStripTraceData.tla beside this module. It defines      *)
(*   TraceNames, TraceBad, TraceTexts, TraceOrder: the constants it walks, *)
(*   TraceSaved: the friend saved as selected at the walk's start, and     *)
(*   Trace: one record per event: what it was (act: list, same, type,      *)
(*     select, restart) and its arguments (list, exact, name, text), then  *)
(*     the code's state after it: the friend tabs in order, their drafts,  *)
(*     the kept drafts, the friend whose tab shows ("" for the sprint      *)
(*     tab), and whether the saved tab waits.                              *)
(*                                                                         *)
(* Each step takes the record's event in FriendStrip.tla; the spec's one   *)
(* free choice, the tab that shows after the one that showed is removed,   *)
(* is the code's. The invariant TraceMatches requires the spec's state to  *)
(* equal the code's after every event, and every FriendStrip property is   *)
(* checked on the trace as well. The walk is one window alone: no other   *)
(* window keeps a draft (OtherTexts is empty), so the file the spec reads  *)
(* at a start is the kept drafts the code hands its next strip.           *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, FriendStripTraceData

VARIABLES strip, drafts, kept, selected, saved, pending, latest, listed, haveList, exact, act, file, others, l

S == INSTANCE FriendStrip WITH Names <- TraceNames, Bad <- TraceBad, Texts <- TraceTexts,
                              SavedOrder <- TraceOrder, MaxList <- 3,
                              OtherTexts <- {}

Matches(k) ==
    LET c == Trace[k].code IN
    /\ strip = c.strip
    /\ drafts = c.drafts
    /\ kept = c.kept
    /\ selected = c.selected
    /\ pending = c.pending

TraceMatches == Matches(l)

traceVars == <<strip, drafts, kept, selected, saved, pending, latest, listed, haveList, exact, act, file, others, l>>

TraceInit == l = 1 /\ S!Init /\ saved = TraceSaved

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ LET s == Trace[l + 1] IN
       /\ \/ s.act = "list" /\ S!List(s.list, s.exact) /\ selected' = s.code.selected
          \/ s.act = "same" /\ S!Same
          \/ s.act = "type" /\ S!Type(s.name, s.text)
          \/ s.act = "select" /\ S!Select(s.name)
          \/ s.act = "restart" /\ S!Restart

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

StripSpec == S!Spec
TypeOK == S!TypeOK
NamesValid == S!NamesValid
DraftsKept == S!DraftsKept
KeptOnlyAbsent == S!KeptOnlyAbsent
StripExact == S!StripExact
StripCovers == S!StripCovers
WaitsOnlyForAbsent == S!WaitsOnlyForAbsent
FileKeepsOthers == S!FileKeepsOthers
SurvivorsKeepOrder == S!SurvivorsKeepOrder
NewInSavedOrder == S!NewInSavedOrder
RestoreSaved == S!RestoreSaved
PersonWins == S!PersonWins

=============================================================================

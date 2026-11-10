---------------------------- MODULE WindowTrace ----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real nexus::Model is *)
(* a behaviour of Window.tla.                                              *)
(*                                                                         *)
(* tests/model/trace_walk.cpp drives the code with a seeded random walk    *)
(* and writes WindowTraceData.tla beside this module. It defines           *)
(*   TraceFriends, TraceIdx, TraceDrafts, TraceBlank, TraceEntries: the    *)
(*     friends the walk constructed the model with and the arguments it    *)
(*     draws from, and                                                     *)
(*   Trace: one record per step. Trace[1] is the constructed model (op     *)
(*     "init"); each later record is the operation called, its arguments,  *)
(*     the value the code returned, and the code's whole state after it.   *)
(*                                                                         *)
(* Each step of this spec takes the Window action the record names, with   *)
(* the record's arguments. The invariant TraceMatches then requires the    *)
(* spec's state and return value to equal what the code recorded. A step   *)
(* whose arguments lie outside the spec's domains has no successor, which  *)
(* TLC reports as a deadlock. The property WindowSpec checks the behaviour *)
(* against Window's Init and Next, and every Window property is checked   *)
(* on the trace as well.                                                   *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, WindowTraceData

VARIABLES friends, active, transcript, draft, last, l

W == INSTANCE Window WITH FriendList <- TraceFriends, Idx <- TraceIdx, Drafts <- TraceDrafts,
                          Blank <- TraceBlank, AppendEntries <- TraceEntries

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

\* the code's per-tab sequences (1-based) as the spec's functions on tab indices (0-based)
AsTabs(s) == [j \in W!Tabs |-> s[j + 1]]

\* the spec's state equals the state the code recorded at step k, and so does the return value
Matches(k) ==
    LET s == Trace[k] IN
    /\ friends = s.friends
    /\ active = s.active
    /\ draft = AsTabs(s.draft)
    /\ transcript = AsTabs(s.transcript)
    /\ last.op = s.op
    /\ last.ok = s.ret

TraceMatches == Matches(l)

\* the Window action a trace record names, with its arguments
Step(s) ==
    /\ s.i \in TraceIdx
    /\ CASE s.op = "select"    -> W!SelectTab(s.i)
         [] s.op = "set_draft" -> s.text \in TraceDrafts /\ W!SetDraft(s.i, s.text)
         [] s.op = "send"      -> W!Send(s.i)
         [] s.op = "queue"     -> W!Queue(s.i)
         [] s.op = "append"    -> s.entry \in TraceEntries /\ W!AppendEntry(s.i, s.entry)
         [] OTHER              -> FALSE

traceVars == <<friends, active, transcript, draft, last, l>>

TraceInit == l = 1 /\ W!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* Window's own spec and properties, checked on the trace
WindowSpec == W!Spec
TypeOK == W!TypeOK
ActiveValid == W!ActiveValid
SelectExact == W!SelectExact
SetDraftExact == W!SetDraftExact
SendResult == W!SendResult
SendAppendsOne == W!SendAppendsOne
SendClearsOnlyItsDraft == W!SendClearsOnlyItsDraft
SendTouchesNoOtherTab == W!SendTouchesNoOtherTab
AppendExact == W!AppendExact
RefusedChangesNothing == W!RefusedChangesNothing
TranscriptsOnlyGrow == W!TranscriptsOnlyGrow
FriendsFixed == W!FriendsFixed

=============================================================================

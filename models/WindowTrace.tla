---------------------------- MODULE WindowTrace ----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real nexus::Model is *)
(* a behaviour of Window.tla.                                              *)
(*                                                                         *)
(* tests/model/trace_walk.cpp drives the code with a seeded random walk    *)
(* and writes WindowTraceData.tla beside this module. It defines           *)
(*   TraceSprint, TraceAIs, TraceArrivals, TraceIdx, TraceDrafts,      *)
(*     TraceBlank, TraceEntries: whether the walk made the model with the  *)
(*     sprint tab, the AIs it made it with, the AIs add_ai is  *)
(*     tried with (empty unless the AIs churn), and the arguments it   *)
(*     draws from, and                                                     *)
(*   Trace: one record per step. Trace[1] is the constructed model (op     *)
(*     "init"); each later record is the operation called, its arguments,  *)
(*     the value the code returned, and the code's whole state after it:   *)
(*     the order (each position's Tab::id), the selection history (ids,    *)
(*     most recent last) and, by position, each tab's kind, AI, draft, *)
(*     transcript and queue; for add, add_ai and remove_ai, the id *)
(*     of the tab; and for add_ai, the AI it was given.            *)
(*                                                                         *)
(* Each step of this spec takes the Window action the record names, with   *)
(* the record's arguments. The invariant TraceMatches then requires the    *)
(* spec's state and return value to equal what the code recorded. A step   *)
(* whose arguments lie outside the spec's domains has no successor, which  *)
(* TLC reports as a deadlock. The property WindowSpec checks the behaviour *)
(* against Window's Init and next-state relation with the AIs coming   *)
(* and going (NextAIsWith), which is Next when TraceArrivals is empty  *)
(* and the trace removes no AI, and every Window property is checked  *)
(* on the trace as well.                                                   *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, WindowTraceData

VARIABLES ais, order, active, transcript, draft, queue, nextId, history, last, l

W == INSTANCE Window WITH AIList <- TraceAIs, Idx <- TraceIdx, Drafts <- TraceDrafts,
                          Blank <- TraceBlank, AppendEntries <- TraceEntries, Sprint <- TraceSprint

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

\* the code's per-position sequence (1-based) as a function on positions (0-based)
AsTabs(s) == [p \in 0 .. (Len(s) - 1) |-> s[p + 1]]

\* the spec's state equals the state the code recorded at step k, and so does the return value. The code records its
\* tabs by position: the order (each position's id) must be the spec's, each tab's kind the kind of its id, an AI's
\* tab must hold the spec's state of that AI (by its id), and the sprint tab's or a terminal's no name, model, draft,
\* transcript or queue. Every AI the spec holds stands in the order (W!NoTabLost), so the AIs match as well.
Matches(k) ==
    LET s == Trace[k] IN
    /\ order = AsTabs(s.order)
    /\ active = s.active
    /\ history = s.history
    /\ \A p \in W!Tabs :
        /\ s.kind[p + 1] = W!TabKind(order[p])
        /\ IF W!TabKind(order[p]) = "AI"
           THEN /\ ais[order[p]] = s.ais[p + 1]
                /\ draft[order[p]] = s.draft[p + 1]
                /\ transcript[order[p]] = s.transcript[p + 1]
                /\ queue[order[p]] = s.queue[p + 1]
           ELSE /\ s.ais[p + 1] = [name |-> "", model |-> ""]
                /\ s.draft[p + 1] = ""
                /\ s.transcript[p + 1] = <<>>
                /\ s.queue[p + 1] = <<>>
    /\ last.op = s.op
    /\ last.ok = s.ret
    /\ last.id = s.id

TraceMatches == Matches(l)

\* the Window action a trace record names, with its arguments
Step(s) ==
    /\ s.i \in TraceIdx
    /\ CASE s.op = "select"    -> W!SelectTab(s.i)
         [] s.op = "set_draft" -> s.text \in TraceDrafts /\ W!SetDraft(s.i, s.text)
         [] s.op = "send"      -> W!Send(s.i)
         [] s.op = "queue"     -> W!Queue(s.i)
         [] s.op = "take"      -> W!TakeQueued(s.i)
         [] s.op = "append"    -> s.entry \in TraceEntries /\ W!AppendEntry(s.i, s.entry)
         [] s.op = "move"      -> s.j \in TraceIdx /\ W!MoveTab(s.i, s.j)
         [] s.op = "add"       -> W!AddTerminal
         [] s.op = "close"     -> W!CloseTab(s.i)
         [] s.op = "add_ai" -> s.ai \in TraceArrivals /\ W!AddAI(s.ai)
         [] s.op = "remove_ai" -> W!RemoveAI(s.i)
         [] OTHER              -> FALSE

traceVars == <<ais, order, active, transcript, draft, queue, nextId, history, last, l>>
windowVars == <<ais, order, active, transcript, draft, queue, nextId, history, last>>

TraceInit == l = 1 /\ W!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* Window's own spec and properties, checked on the trace
WindowSpec == W!Init /\ [][W!NextAIsWith(TraceArrivals)]_windowVars
TypeOK == W!TypeOK
ActiveValid == W!ActiveValid
SelectExact == W!SelectExact
SetDraftExact == W!SetDraftExact
SendResult == W!SendResult
SendAppendsOne == W!SendAppendsOne
QueueAppendsOne == W!QueueAppendsOne
QueueLeavesTranscripts == W!QueueLeavesTranscripts
OnlyQueueTouchesQueues == W!OnlyQueueTouchesQueues
TakeExact == W!TakeExact
SendClearsOnlyItsDraft == W!SendClearsOnlyItsDraft
SendTouchesNoOtherTab == W!SendTouchesNoOtherTab
AppendExact == W!AppendExact
NoTabLost == W!NoTabLost
NoTabDuplicated == W!NoTabDuplicated
IdsHandedOut == W!IdsHandedOut
ZonesOrdered == W!ZonesOrdered
SprintFirst == W!SprintFirst
TerminalTakesNoText == W!TerminalTakesNoText
SprintTakesNoText == W!SprintTakesNoText
SprintNeverMoves == W!SprintNeverMoves
SprintNeverCloses == W!SprintNeverCloses
AddAppends == W!AddAppends
AddFreshId == W!AddFreshId
NextIdGrows == W!NextIdGrows
CloseRemovesExactly == W!CloseRemovesExactly
CloseKeepsAIs == W!CloseKeepsAIs
CloseActiveRule == W!CloseActiveRule
SelectMovesToEnd == W!SelectMovesToEnd
CloseForgets == W!CloseForgets
HistoryOpen == W!HistoryOpen
HistoryOnce == W!HistoryOnce
HistoryEndsActive == W!HistoryEndsActive
MovePlaces == W!MovePlaces
MoveKeepsTabs == W!MoveKeepsTabs
OnlyMoveReorders == W!OnlyMoveReorders
ActiveAIKept == W!ActiveAIKept
RefusedChangesNothing == W!RefusedChangesNothing
TranscriptsOnlyGrow == W!TranscriptsOnlyGrow
QueuesOnlyGrow == W!QueuesOnlyGrow
AIsFixed == W!AIsFixed
NamesOnce == W!NamesOnce
AddAIExact == W!AddAIExact
RemoveAIExact == W!RemoveAIExact
AIOpsKeepOthers == W!AIOpsKeepOthers
RemoveForgets == W!RemoveForgets
RemoveActiveRule == W!RemoveActiveRule

=============================================================================

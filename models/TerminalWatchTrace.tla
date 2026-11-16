------------------------- MODULE TerminalWatchTrace -------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of src/terminal_watch.h     *)
(* (written by tests/model/watch_walk.cpp to TerminalWatchTraceData.tla)   *)
(* is a behaviour of TerminalWatch.tla.                                    *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, TerminalWatchTraceData

VARIABLES osGroup, shared, seen, recheck, poll, frame, changes, titles, last, l

W == INSTANCE TerminalWatch WITH
    Groups <- TraceGroups,
    SshGroups <- TraceSshGroups,
    MaxChanges <- Len(Trace),
    MaxTitles <- Len(Trace)

traceVars == <<osGroup, shared, seen, recheck, poll, frame, changes, titles, last, l>>

Matches(k) ==
    LET s == Trace[k] IN
    /\ osGroup = s.osGroup
    /\ shared = s.shared
    /\ seen = s.seen
    /\ recheck = s.recheck
    /\ poll.active = s.pollActive
    /\ frame.active = s.frameActive
    /\ (k > 1 => (last.op = s.op /\ last.changed = s.changed /\ last.recheck = s.askedRecheck))

TraceMatches == Matches(l)

TraceInit ==
    /\ l = 1
    /\ W!Init
    /\ osGroup = Trace[1].osGroup

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ LET s == Trace[l + 1] IN
       CASE s.op = "env"          -> W!EnvChange /\ osGroup' = s.osGroup
         [] s.op = "frame-take"   -> W!FrameTake
         [] s.op = "frame-start"  -> W!FrameStart
         [] s.op = "frame-commit" -> W!FrameCommit
         [] s.op = "poll-start"   -> W!PollStart
         [] s.op = "poll-commit"  -> W!PollCommit
         [] OTHER                 -> FALSE

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

WatchSpec == W!SafeSpec
TypeOK == W!TypeOK
SeqOrdered == W!SeqOrdered
RemoteMatchesGroup == W!RemoteMatchesGroup
SeqMonotone == W!SeqMonotone
FrameSeesTitleGroup == W!FrameSeesTitleGroup
PollNeverClobbersFrame == W!PollNeverClobbersFrame
OverlapAsksRecheck == W!OverlapAsksRecheck

=============================================================================

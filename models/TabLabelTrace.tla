--------------------------- MODULE TabLabelTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real nexus::TabLabel *)
(* is a behaviour of TabLabel.tla.                                         *)
(*                                                                         *)
(* tests/model/label_walk.cpp drives the code with a seeded random walk    *)
(* and writes TabLabelTraceData.tla beside this module. It defines         *)
(*   TraceTitles, TraceRemotes: the titles and remote labels it draws, and *)
(*   Trace: one record per observation: its arguments (remote, title, and  *)
(*     late, whether the frame's time is past the tail of the last session *)
(*     to end, as the walk reckons it from its own clock) and the code's   *)
(*     state after it, with the labels the code drew for every title.      *)
(*                                                                         *)
(* Each step takes TabLabel's Observe with the record's arguments; the     *)
(* invariant TraceMatches requires the spec's state, and the label for     *)
(* every title, to equal the code's. Every TabLabel property is checked on *)
(* the trace as well.                                                      *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, TabLabelTraceData

VARIABLES remote, before, after, masking, tail, last, l

T == INSTANCE TabLabel WITH Titles <- TraceTitles, Remotes <- TraceRemotes

\* the spec's state and labels equal what the code recorded at step k (step 1 is the code before any observation)
Matches(k) ==
    LET s == Trace[k] IN
    /\ remote = s.code.remote
    /\ before = s.code.before
    /\ after = s.code.after
    /\ masking = s.code.masking
    /\ tail = s.code.tail
    /\ \A t \in DOMAIN s.labels : T!Label(t) = s.labels[t]

TraceMatches == Matches(l)

traceVars == <<remote, before, after, masking, tail, last, l>>

TraceInit == l = 1 /\ T!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ LET s == Trace[l + 1] IN
       /\ s.remote \in TraceRemotes \cup {T!NoRemote}
       /\ s.title \in TraceTitles
       /\ T!Observe(s.remote, s.title, s.late)

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

LabelSpec == T!Spec
TypeOK == T!TypeOK
TailMasks == T!TailMasks
SessionNamed == T!SessionNamed
TitleShows == T!TitleShows
SessionNamedAtOnce == T!SessionNamedAtOnce
EndRestores == T!EndRestores
TailRestores == T!TailRestores
MaskEnds == T!MaskEnds
BeforeIsOwn == T!BeforeIsOwn
BeforeIsLatest == T!BeforeIsLatest

=============================================================================

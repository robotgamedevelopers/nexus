--------------------------- MODULE ClientLockTrace ---------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real                 *)
(* nexus::ClientLock is a behaviour of ClientLock.tla.                     *)
(*                                                                         *)
(* tests/model/lock_walk.cpp drives the lock with a seeded random walk,    *)
(* with a stand-in authenticator that answers every attempt it was given,  *)
(* late ones included, and the OS's relock events, and writes              *)
(* ClientLockTraceData.tla beside this module. It defines Trace: one       *)
(* record per step. Trace[1] is the start (op "init"); each later record   *)
(* is the step (op, its argument, attempt and seconds, and the code's      *)
(* answer: a frame's drawsContent(), an input's delivery) and the lock's   *)
(* state after it: state, why, attempt, and the idle seconds.              *)
(*                                                                         *)
(* Each step takes the ClientLock action the record names, with the        *)
(* record's argument. The invariant TraceMatches requires the spec's state *)
(* and history to equal the code's; a step the spec cannot take is a       *)
(* deadlock.                                                               *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, ClientLockTraceData

CONSTANTS Timeout, MaxAttempt

VARIABLES state, why, attempt, pending, idle, granted, last, l

L == INSTANCE ClientLock

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET s == Trace[k] IN
    /\ state = s.state
    /\ why = s.why
    /\ attempt = s.attempt
    /\ idle = s.idle
    /\ last = L!Rec(s.op, s.arg, s.n, s.d, s.flag)

TraceMatches == Matches(l)

\* the ClientLock action a trace record names, with its argument
Step(s) ==
    CASE s.op = "prompt" /\ s.n # 0 -> L!Prompt
      [] s.op = "prompt"            -> L!PromptIgnored
      [] s.op = "answer"            -> L!Answer(s.n, s.arg)
      [] s.op = "relock"            -> L!Relock(s.arg)
      [] s.op = "tick"              -> L!Tick(s.d)
      [] s.op = "input"             -> L!Input
      [] s.op = "frame"             -> L!Frame
      [] OTHER                      -> FALSE

traceVars == <<state, why, attempt, pending, idle, granted, last, l>>

TraceInit == l = 1 /\ L!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

\* the end of the trace: stutter, so that a trace read to its end is no deadlock
Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* ClientLock's spec (without its fairness, which a finite trace cannot meet) and its properties, checked on the trace
LockSpec == L!SafeSpec
TypeOK == L!TypeOK
UnlockedByAuth == L!UnlockedByAuth
UnlockedSaysNothing == L!UnlockedSaysNothing
IdleOnlyUnlocked == L!IdleOnlyUnlocked
IdleBelowTimeout == L!IdleBelowTimeout
PromptPending == L!PromptPending
ContentOnlyUnlocked == L!ContentOnlyUnlocked
InputOnlyUnlocked == L!InputOnlyUnlocked
UnlockOnlyByCurrent == L!UnlockOnlyByCurrent
StaleIgnored == L!StaleIgnored
RefusalSaysWhy == L!RefusalSaysWhy
RelockLocks == L!RelockLocks
IdleLocks == L!IdleLocks
PromptOnlyLocked == L!PromptOnlyLocked

=============================================================================

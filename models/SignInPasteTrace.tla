-------------------------- MODULE SignInPasteTrace --------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the paste path's code    *)
(* (src/sync/signin.h SignInDesk: need, claim, url, opened, codeWanted,    *)
(* code, printed, exited, expire, windowGone, drop, and offersFor as each  *)
(* window's view) is a behaviour of SignInPaste.tla.                       *)
(*                                                                         *)
(* tests/sync/signin_paste_walk.cpp drives the desk with a seeded random   *)
(* walk, playing the enrollment process, the windows (two, one of them on  *)
(* another machine), the person and stray or hostile windows, and writes   *)
(* SignInPasteTraceData.tla beside this module: the instance and Trace,    *)
(* one record per step, each the step (op and its arguments) and then the  *)
(* code's whole state as the model's variables, all but log (the walk      *)
(* fails at once on any log line holding a code or a URL).                 *)
(*                                                                         *)
(* Each step of this spec takes the action the record names, and           *)
(* TraceMatches requires the spec's state to equal the code's. A step the  *)
(* spec cannot take is a deadlock. The rules are checked on the code's own  *)
(* record first (the Code ones), so a broken rule is named as such.        *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, SignInPasteTraceData

VARIABLES next, flow, proc, conn, view, claimed, opened, pasted, up, strays, signedin, restarts, sent, badfrom, unasked,
          log, l

S == INSTANCE SignInPaste WITH Clients <- TraceClients, Remote <- TraceRemote, Codes <- TraceCodes,
                               MaxFlows <- TraceMaxFlows, MaxStrays <- TraceMaxStrays, MaxRestarts <- TraceMaxRestarts

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

MatchServer == next = Trace[l].next /\ flow = Trace[l].flow /\ proc = Trace[l].proc /\ signedin = Trace[l].signedin /\
               restarts = Trace[l].restarts
MatchWindows == conn = Trace[l].conn /\ claimed = Trace[l].claimed /\ opened = Trace[l].opened /\
                pasted = Trace[l].pasted /\ strays = Trace[l].strays
MatchViews == view = Trace[l].view
MatchUp == up = Trace[l].up
MatchGhosts == sent = Trace[l].sent /\ badfrom = Trace[l].badfrom /\ unasked = Trace[l].unasked
TraceMatches == MatchServer /\ MatchWindows /\ MatchViews /\ MatchUp /\ MatchGhosts

Step(r) ==
    CASE r.op = "need"       -> S!Need
      [] r.op = "receive"    -> S!Receive(r.m)
      [] r.op = "url"        -> S!Url
      [] r.op = "ask"        -> S!Ask
      [] r.op = "enroll"     -> S!Enroll
      [] r.op = "exit"       -> S!Exit
      [] r.op = "timeout"    -> S!Timeout(r.i)
      [] r.op = "restart"    -> S!Restart
      [] r.op = "sync"       -> S!Sync(r.c)
      [] r.op = "claim"      -> S!Claim(r.c) /\ r.i \in claimed'[r.c] \ claimed[r.c]
      [] r.op = "open"       -> S!Open(r.c) /\ r.i \in opened'[r.c] \ opened[r.c]
      [] r.op = "paste"      -> S!Paste(r.c, r.v) /\ r.i \in pasted'[r.c] \ pasted[r.c]
      [] r.op = "stray"      -> S!Stray(r.c, r.i, r.v)
      [] r.op = "disconnect" -> S!Disconnect(r.c)
      [] r.op = "connect"    -> S!Connect(r.c)
      [] OTHER               -> FALSE

traceVars == <<next, flow, proc, conn, view, claimed, opened, pasted, up, strays, signedin, restarts, sent, badfrom,
              unasked, log, l>>

TraceInit == l = 1 /\ S!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* The rules on the code's own record.
C == INSTANCE SignInPaste WITH Clients <- TraceClients, Remote <- TraceRemote, Codes <- TraceCodes,
                               MaxFlows <- TraceMaxFlows, MaxStrays <- TraceMaxStrays, MaxRestarts <- TraceMaxRestarts,
                               flow <- Trace[l].flow, proc <- Trace[l].proc, view <- Trace[l].view,
                               sent <- Trace[l].sent, badfrom <- Trace[l].badfrom, unasked <- Trace[l].unasked
CodeOneCodePerFlow == C!OneCodePerFlow
CodeCodeOnlyFromHolder == C!CodeOnlyFromHolder
CodeCodeOnlyWhenAsked == C!CodeOnlyWhenAsked
CodeNoCodeToWindows == C!NoCodeToWindows
CodeAskOnlyToHolder == C!AskOnlyToHolder
CodeProcessHasItsFlow == C!ProcessHasItsFlow

\* SignInPaste's properties, checked on the trace.
TypeOK == S!TypeOK
OneFlow == S!OneFlow
ProcessHasItsFlow == S!ProcessHasItsFlow
OneCodePerFlow == S!OneCodePerFlow
CodeOnlyFromHolder == S!CodeOnlyFromHolder
CodeOnlyWhenAsked == S!CodeOnlyWhenAsked
NoCodeToWindows == S!NoCodeToWindows
AskOnlyToHolder == S!AskOnlyToHolder
EndedStaysEnded == S!EndedStaysEnded
=============================================================================

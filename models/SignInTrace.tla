---------------------------- MODULE SignInTrace ----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of in-app sign-in's code    *)
(* (src/sync/signin.h through src/sync/hub.h, with each window's           *)
(* SignInOpener and CallbackGate) is a behaviour of SignIn.tla.            *)
(*                                                                         *)
(* tests/sync/signin_walk.cpp drives the code with a seeded random walk,  *)
(* playing the processes, the browsers, the person, the links, the disk,   *)
(* the clock and the restarts, and writes SignInTraceData.tla beside this  *)
(* module: the instance and Trace, one record per step. Trace[1] is the    *)
(* start (op "init"); each later record is the step (op, and its           *)
(* arguments), then the code's whole state as the model's variables, all   *)
(* but log (the walk fails at once on any log line that holds a value).   *)
(*                                                                         *)
(* Each step of this spec takes the SignIn action the record names, and    *)
(* TraceMatches requires the spec's state to equal the code's, variable by *)
(* variable. A step the spec cannot take is a deadlock. The properties are *)
(* checked on the code's own record first (the Code ones), so TLC names a  *)
(* broken rule rather than the mismatch it also is, then on the trace.    *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, SignInTraceData

VARIABLES next, flow, proc, procs, cbs, badcb, signedin, losses, conn, armed, view, claimed, opened, browser, gate, up,
          file, held, disk, failures, taken, lost, restarts, said, log, ask, l

S == INSTANCE SignIn WITH Clients <- TraceClients, Remote <- TraceRemote, Accounts <- TraceAccounts,
                          Tokens <- TraceTokens, MaxFlows <- TraceMaxFlows, MaxRestarts <- TraceMaxRestarts,
                          MaxFailures <- TraceMaxFailures, MaxLosses <- TraceMaxLosses

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET r == Trace[k] IN
    /\ next = r.next
    /\ flow = r.flow
    /\ proc = r.proc
    /\ procs = r.procs
    /\ cbs = r.cbs
    /\ badcb = r.badcb
    /\ signedin = r.signedin
    /\ losses = r.losses
    /\ conn = r.conn
    /\ armed = r.armed
    /\ view = r.view
    /\ claimed = r.claimed
    /\ opened = r.opened
    /\ browser = r.browser
    /\ gate = r.gate
    /\ up = r.up
    /\ file = r.file
    /\ held = r.held
    /\ disk = r.disk
    /\ failures = r.failures
    /\ taken = r.taken
    /\ lost = r.lost
    /\ restarts = r.restarts
    /\ said = r.said
    /\ ask = r.ask

TraceMatches == Matches(l)

\* TraceMatches by variable, before it in SignInTrace.cfg, so that a mismatch names the variable the code differs in.
MatchServer == next = Trace[l].next /\ flow = Trace[l].flow /\ proc = Trace[l].proc /\ procs = Trace[l].procs /\
               signedin = Trace[l].signedin /\ losses = Trace[l].losses /\ ask = Trace[l].ask
MatchCallbacks == cbs = Trace[l].cbs /\ badcb = Trace[l].badcb
MatchWindows == conn = Trace[l].conn /\ armed = Trace[l].armed /\ claimed = Trace[l].claimed /\
                opened = Trace[l].opened /\ browser = Trace[l].browser
MatchViews == view = Trace[l].view
MatchGates == gate = Trace[l].gate
MatchUp == up = Trace[l].up
MatchStore == file = Trace[l].file /\ held = Trace[l].held /\ disk = Trace[l].disk /\ failures = Trace[l].failures /\
              taken = Trace[l].taken /\ lost = Trace[l].lost /\ restarts = Trace[l].restarts /\
              said = Trace[l].said

Step(r) ==
    CASE r.op = "need"       -> S!Need(r.a)
      [] r.op = "lose"       -> S!Lose(r.a)
      [] r.op = "receive"    -> S!Receive(r.m)
      [] r.op = "url"        -> S!Url(r.a)
      [] r.op = "print"      -> S!Print(r.a, r.v)
      [] r.op = "exit"       -> S!Exit(r.a)
      [] r.op = "end"        -> S!End(r.a)
      [] r.op = "timeout"    -> S!Timeout(r.id)
      [] r.op = "fallback"   -> S!FallBack(r.id)
      [] r.op = "retry"      -> S!Retry(r.a)
      [] r.op = "askend"     -> S!AskEnds(r.a)
      [] r.op = "diskfail"   -> S!DiskFails
      [] r.op = "diskheal"   -> S!DiskHeals
      [] r.op = "restart"    -> S!Restart
      [] r.op = "show"       -> S!Show(r.c)
      [] r.op = "hide"       -> S!Hide(r.c)
      [] r.op = "sync"       -> S!Sync(r.c)
      [] r.op = "claim"      -> S!Claim(r.c)
      [] r.op = "open"       -> S!Open(r.c) /\ r.i \in opened'[r.c] \ opened[r.c]
      [] r.op = "redirect"   -> S!Redirect(r.c, r.i, r.ok)
      [] r.op = "cancel"     -> S!Cancel(r.c, r.i)
      [] r.op = "stray"      -> S!Stray(r.c)
      [] r.op = "disconnect" -> S!Disconnect(r.c)
      [] r.op = "connect"    -> S!Connect(r.c)
      [] OTHER               -> FALSE

traceVars == <<next, flow, proc, procs, cbs, badcb, signedin, losses, conn, armed, view, claimed, opened, browser, gate,
              up, file, held, disk, failures, taken, lost, restarts, said, log, ask, l>>

TraceInit == l = 1 /\ S!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* The rules on the code's own record.
C == INSTANCE SignIn WITH Clients <- TraceClients, Remote <- TraceRemote, Accounts <- TraceAccounts,
                          Tokens <- TraceTokens, MaxFlows <- TraceMaxFlows, MaxRestarts <- TraceMaxRestarts,
                          MaxFailures <- TraceMaxFailures, MaxLosses <- TraceMaxLosses,
                          procs <- Trace[l].procs, cbs <- Trace[l].cbs, badcb <- Trace[l].badcb, up <- Trace[l].up,
                          view <- Trace[l].view, flow <- Trace[l].flow
CodeOneProcessPerAccount == C!OneProcessPerAccount
CodeOneCallbackPerFlow == C!OneCallbackPerFlow
CodeCallbackOnlyMatching == C!CallbackOnlyMatching
CodeGateForwardsOnlyMatching == C!GateForwardsOnlyMatching
CodeNoTokenToWindows == C!NoTokenToWindows
CodeOneFlowPerAccount == C!OneFlowPerAccount

\* SignIn's properties, checked on the trace.
TypeOK == S!TypeOK
OneFlowPerAccount == S!OneFlowPerAccount
OneProcessPerAccount == S!OneProcessPerAccount
ProcessHasItsFlow == S!ProcessHasItsFlow
OneCallbackPerFlow == S!OneCallbackPerFlow
CallbackOnlyMatching == S!CallbackOnlyMatching
GateForwardsOnlyMatching == S!GateForwardsOnlyMatching
TokenNeverDropped == S!TokenNeverDropped
HeldOnlyCaptured == S!HeldOnlyCaptured
EveryPrintTaken == S!EveryPrintTaken
GateForEveryPage == S!GateForEveryPage
SignedInHasLogin == S!SignedInHasLogin
NoAskWhenSignedIn == S!NoAskWhenSignedIn
NoTokenToWindows == S!NoTokenToWindows
ProcessOnlyFromNeeded == S!ProcessOnlyFromNeeded
EndedStaysEnded == S!EndedStaysEnded

=============================================================================

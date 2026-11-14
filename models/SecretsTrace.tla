---------------------------- MODULE SecretsTrace ----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the secret dialog's code *)
(* (src/sync/secrets.h through src/sync/hub.h, with each window's          *)
(* SecretPrompt) is a behaviour of Secrets.tla.                            *)
(*                                                                         *)
(* tests/sync/secrets_walk.cpp drives the code with a seeded random walk, *)
(* playing the askers, the windows, the person, the clock and the server's *)
(* restarts, and writes SecretsTraceData.tla beside this module. It        *)
(* defines the instance (TraceClients, TraceOldClients, TraceAccounts,     *)
(* TraceAskers, TraceValues, TraceMaxRequests, TraceMaxRestarts) and       *)
(* Trace: one record per step. Trace[1] is the start (op "init"); each     *)
(* later record is the event the code was given (op, and its arguments),   *)
(* then the code's whole state as the model's variables.                   *)
(*                                                                         *)
(* Each step of this spec takes the Secrets action the record names, and   *)
(* TraceMatches requires the spec's state to equal the code's, variable by *)
(* variable. A step the spec cannot take is a deadlock. The model's safety *)
(* properties are checked on the trace.                                    *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, SecretsTraceData

VARIABLES next, req, conn, view, dialog, done, up, file, stores, replies, transcript, log, restarts, l

S == INSTANCE Secrets WITH Clients <- TraceClients, OldClients <- TraceOldClients, Accounts <- TraceAccounts,
                           Askers <- TraceAskers, Values <- TraceValues, MaxRequests <- TraceMaxRequests,
                           MaxRestarts <- TraceMaxRestarts

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET r == Trace[k] IN
    /\ next = r.next
    /\ req = r.req
    /\ conn = r.conn
    /\ view = r.view
    /\ dialog = r.dialog
    /\ done = r.done
    /\ up = r.up
    /\ file = r.file
    /\ stores = r.stores
    /\ replies = r.replies
    /\ transcript = r.transcript
    /\ log = r.log
    /\ restarts = r.restarts

TraceMatches == Matches(l)

Step(r) ==
    CASE r.op = "ask"        -> S!Ask(r.asker, r.acct)
      [] r.op = "sync"       -> S!Sync(r.c)
      [] r.op = "type"       -> S!Type(r.c, r.v)
      [] r.op = "submit"     -> S!Submit(r.c)
      [] r.op = "cancel"     -> S!Cancel(r.c)
      [] r.op = "disconnect" -> S!Disconnect(r.c)
      [] r.op = "connect"    -> S!Connect(r.c)
      [] r.op = "receive"    -> S!Receive(r.m)
      [] r.op = "timeout"    -> S!Timeout(r.id)
      [] r.op = "restart"    -> S!Restart
      [] OTHER               -> FALSE

traceVars == <<next, req, conn, view, dialog, done, up, file, stores, replies, transcript, log, restarts, l>>

TraceInit == l = 1 /\ S!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* Secrets' properties, checked on the trace.
TypeOK == S!TypeOK
NoSecretToAsker == S!NoSecretToAsker
NoSecretInTranscript == S!NoSecretInTranscript
NoSecretInLog == S!NoSecretInLog
NoSecretToWindows == S!NoSecretToWindows
AtMostOneStored == S!AtMostOneStored
StoredWhenStored == S!StoredWhenStored
OneOpenPerAccount == S!OneOpenPerAccount
AnsweredOnce == S!AnsweredOnce
AnsweredWhenEnded == S!AnsweredWhenEnded
ClosedStaysClosed == S!ClosedStaysClosed
AskNeedsTaker == S!AskNeedsTaker

=============================================================================

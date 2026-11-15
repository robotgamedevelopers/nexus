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
(* TraceAskers, TraceValues, TraceBadValues, TraceMaxRequests,             *)
(* TraceMaxRestarts, TraceMaxFailures) and                                 *)
(* Trace: one record per step. Trace[1] is the start (op "init"); each     *)
(* later record is the event the code was given (op, and its arguments),   *)
(* then the code's whole state as the model's variables.                   *)
(*                                                                         *)
(* Each step of this spec takes the Secrets action the record names, and   *)
(* TraceMatches requires the spec's state to equal the code's, variable by *)
(* variable. A step the spec cannot take is a deadlock. Where the spec     *)
(* leaves a choice open (a write that was not durable may or may not have  *)
(* reached the file), the code's record picks it (FileChosen). The         *)
(* model's safety properties are checked on the trace.                     *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, SecretsTraceData

VARIABLES next, req, conn, view, dialog, done, up, file, stores, replies, transcript, log, restarts, disk, failures,
          held, taken, lost, l

S == INSTANCE Secrets WITH Clients <- TraceClients, OldClients <- TraceOldClients, Accounts <- TraceAccounts,
                           Askers <- TraceAskers, Values <- TraceValues, MaxRequests <- TraceMaxRequests,
                           MaxRestarts <- TraceMaxRestarts, BadValues <- TraceBadValues,
                           MaxFailures <- TraceMaxFailures

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
    /\ disk = r.disk
    /\ failures = r.failures
    /\ held = r.held
    /\ taken = r.taken
    /\ lost = r.lost

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
      [] r.op = "retry"      -> S!Retry(r.id) \/ S!RetryFails(r.id)
      [] r.op = "diskfail"   -> S!DiskFails
      [] r.op = "diskheal"   -> S!DiskHeals
      [] OTHER               -> FALSE

traceVars == <<next, req, conn, view, dialog, done, up, file, stores, replies, transcript, log, restarts, disk,
              failures, held, taken, lost, l>>

TraceInit == l = 1 /\ S!Init

\* The request a receive or a retry is for.
StepId(r) == IF r.op = "receive" THEN r.m.id ELSE r.id

\* A write that was not durable (S!Hold, S!RetryFails) leaves the file as it was or holding the value: the spec allows
\* both, and the code took one. On such a step (a receive or a retry that leaves its request holding) the code's file
\* picks the branch; a file the spec does not allow leaves no step, a deadlock. On every other step the file is checked
\* by TraceMatches like any variable, so a late answer stored anyway still fails TraceMatches.
FileChosen(r) ==
    (r.op \in {"receive", "retry"} /\ req'[StepId(r)].st = "holding") => file' = r.file

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])
    /\ FileChosen(Trace[l + 1])

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* The no-value properties again, on the code's own state: each record's replies, transcript, log and windows' views
\* are built from the bytes each sink received (the driver line and the reason, the session lines and the store's
\* entries, the server.log line, each diff sent to a window), with any value found in them written as the value's
\* model value (tests/leak_scan.h finds it). So a value that reaches a sink in code violates these, whatever the spec
\* allows; they come before TraceMatches in SecretsTrace.cfg, so TLC names the leak rather than the mismatch.
C == INSTANCE Secrets WITH Clients <- TraceClients, OldClients <- TraceOldClients, Accounts <- TraceAccounts,
                           Askers <- TraceAskers, Values <- TraceValues, MaxRequests <- TraceMaxRequests,
                           MaxRestarts <- TraceMaxRestarts, BadValues <- TraceBadValues,
                           MaxFailures <- TraceMaxFailures, replies <- Trace[l].replies,
                           transcript <- Trace[l].transcript, log <- Trace[l].log, view <- Trace[l].view

CodeNoSecretToAsker == C!NoSecretToAsker
CodeNoSecretInTranscript == C!NoSecretInTranscript
CodeNoSecretInLog == C!NoSecretInLog
CodeNoSecretToWindows == C!NoSecretToWindows

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
ReceivedNeverDropped == S!ReceivedNeverDropped
StoredOnlyDurable == S!StoredOnlyDurable
HeldOnlyWhileHolding == S!HeldOnlyWhileHolding
NoMalformedStored == S!NoMalformedStored
HeldOnlyStored == S!HeldOnlyStored

=============================================================================

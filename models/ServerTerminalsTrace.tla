------------------------- MODULE ServerTerminalsTrace -------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the server's terminals   *)
(* (TerminalBook, sync::Applier and sync::terminalFate, driven by          *)
(* tests/model/terminals_walk.cpp, which writes                            *)
(* ServerTerminalsTraceData.tla) is a behaviour of ServerTerminals.tla.    *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, ServerTerminalsTraceData

VARIABLES epoch, term, outbox, applied, linked, delivered, seen, seenEp, restarts, l

T == INSTANCE ServerTerminals WITH
    Clients <- TraceClients,
    Slots <- TraceSlots,
    MaxSends <- TraceMaxSends,
    MaxRestarts <- TraceRestarts

traceVars == <<epoch, term, outbox, applied, linked, delivered, seen, seenEp, restarts, l>>

Sends == {"create", "input", "resize", "close"}

\* The model's state is the code's: the run, which terminals the book holds and how (a slot the book does not hold is
\* closed, lost or never made), each window's counter, link and actions, what each shell was given, and each tab.
Matches(k) ==
    LET s == Trace[k] IN
    /\ epoch = s.epoch
    /\ \A x \in TraceSlots :
           IF s.live[x] = "none" THEN ~T!Live(x)
           ELSE T!Live(x) /\ term[x].state = s.live[x] /\ term[x].size = s.size[x]
    /\ \A c \in TraceClients :
           /\ applied[c] = s.applied[c]
           /\ linked[c] = s.linked[c]
           /\ Len(outbox[c]) = s.sent[c]
           /\ seen[c] = s.seen[c]
           /\ seenEp[c] = s.seenEp[c]
    /\ \A x \in TraceSlots : delivered[x] = s.delivered[x]
    /\ (s.op \in Sends => outbox[s.c][Len(outbox[s.c])] = s.act)

TraceMatches == Matches(l)

TraceInit ==
    /\ l = 1
    /\ T!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ LET s == Trace[l + 1] IN
       CASE s.op = "create"  -> T!SendCreate(s.c)
         [] s.op = "input"   -> T!SendInput(s.c, s.x)
         [] s.op = "resize"  -> T!SendResize(s.c, s.x, s.act.size)
         [] s.op = "close"   -> T!SendClose(s.c, s.x)
         \* The book's id says which slot a create took; the model may take any free one.
         [] s.op = "apply"   -> T!Apply(s.c) /\ (s.spawned # 0 => term'[s.spawned].state = "running" /\
                                                                    term'[s.spawned].ep = epoch)
         [] s.op = "exit"    -> T!Exit(s.x)
         [] s.op = "detach"  -> T!Detach(s.c)
         [] s.op = "attach"  -> T!Attach(s.c)
         [] s.op = "restart" -> T!Restart
         [] s.op = "sync"    -> T!Sync(s.c)
         [] s.op = "dismiss" -> T!Dismiss(s.c, s.x)
         [] OTHER            -> FALSE

\* The end of the trace, and a step the model cannot take: the state stays, so TraceMatches judges the next step's (every
\* step the walk records changes what Matches compares: a send the count, an apply the counter, an exit or a restart the
\* terminals, a link its flag, a sync or a dismiss a tab; so a step the model cannot take never matches by staying).
Done == l < Len(Trace) => ~ENABLED TraceNext
Stay ==
    /\ Done
    /\ l' = (IF l < Len(Trace) THEN l + 1 ELSE l)
    /\ UNCHANGED <<epoch, term, outbox, applied, linked, delivered, seen, seenEp, restarts>>

TraceSpec == TraceInit /\ [][TraceNext \/ Stay]_traceVars

TypeOK == T!TypeOK
InputOnceInOrder == T!InputOnceInOrder
InputOnlyToItsRun == T!InputOnlyToItsRun
ModelSpec == T!SafeSpec
NoInputUnlessRunning == T!NoInputUnlessRunning
DetachKeepsRunning == T!DetachKeepsRunning
RestartShown == T!RestartShown

=============================================================================

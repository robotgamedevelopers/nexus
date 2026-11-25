--------------------------- MODULE ServerTerminals ---------------------------
(***************************************************************************)
(* The server's terminals (docs/terminals.md, src/server/terminal_book.h,  *)
(* src/server/terminal_host.h, src/sync/terminal_state.h): a terminal is   *)
(* a shell nexus-server runs; windows (Clients) create, type into, resize  *)
(* and close it through their numbered actions, link and unlink at will,   *)
(* and see it through the state the server sends.                          *)
(*                                                                         *)
(* What it keeps:                                                          *)
(*   - a window's actions are applied once each, in number order, by the   *)
(*     store's durable counter (`applied`), across unlinking and the       *)
(*     server's restart (sync/actions.h Applier);                          *)
(*   - input reaches a terminal only while its shell runs, only the run    *)
(*     (epoch) it was typed for, once and in order (InputOnceInOrder,      *)
(*     InputOnlyToItsRun, NoInputUnlessRunning);                           *)
(*   - a window unlinking changes no terminal (DetachKeepsRunning);        *)
(*   - a shell's end is shown to every linked window (ExitShown), and a    *)
(*     terminal lost to a restart is shown so, never dropped silently      *)
(*     (RestartNeverSilent, RestartShown).                                 *)
(*                                                                         *)
(* A terminal's identity is its slot and the epoch it was made in: the     *)
(* code's ids hold the epoch in their high 32 bits, so a slot of another   *)
(* epoch is another terminal.                                              *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Clients, Slots, MaxSends, MaxRestarts

States == {"none", "running", "ended", "closed", "lost"}
Seen == {"absent", "running", "ended", "restarted"}
Sizes == {1, 2}

Act == [kind : {"create"}, slot : {0}, ep : {0}, key : {0}, size : {0}]
    \cup [kind : {"input"}, slot : Slots, ep : 0..MaxRestarts, key : 1..MaxSends, size : {0}]
    \cup [kind : {"resize"}, slot : Slots, ep : 0..MaxRestarts, key : {0}, size : Sizes]
    \cup [kind : {"close"}, slot : Slots, ep : 0..MaxRestarts, key : {0}, size : {0}]

VARIABLES
    epoch,      \* the run of the server: a restart starts the next
    term,       \* per slot: its state, the epoch it was made in, its screen's size
    outbox,     \* per window: every action it sent, in number order (mosh carries them again until acknowledged)
    applied,    \* per window: the number of its last action applied, durable in the store
    linked,     \* per window: its link is up
    delivered,  \* per slot: the input its shell was given, in order: [c, key, ep]
    seen,       \* per window, per slot: what its tab shows
    seenEp,     \* per window, per slot: the epoch of the terminal its tab shows
    restarts

vars == <<epoch, term, outbox, applied, linked, delivered, seen, seenEp, restarts>>

Live(s) == term[s].state \in {"running", "ended"} /\ term[s].ep = epoch

TypeOK ==
    /\ epoch \in 0..MaxRestarts
    /\ term \in [Slots -> [state : States, ep : 0..MaxRestarts, size : Sizes]]
    /\ \A c \in Clients : Len(outbox[c]) <= MaxSends /\ \A i \in 1..Len(outbox[c]) : outbox[c][i] \in Act
    /\ applied \in [Clients -> 0..MaxSends]
    /\ linked \in [Clients -> BOOLEAN]
    /\ \A s \in Slots : \A i \in 1..Len(delivered[s]) :
           delivered[s][i] \in [c : Clients, key : 1..MaxSends, ep : 0..MaxRestarts]
    /\ seen \in [Clients -> [Slots -> Seen]]
    /\ seenEp \in [Clients -> [Slots -> 0..MaxRestarts]]
    /\ restarts \in 0..MaxRestarts

Init ==
    /\ epoch = 0
    /\ term = [s \in Slots |-> [state |-> "none", ep |-> 0, size |-> 1]]
    /\ outbox = [c \in Clients |-> <<>>]
    /\ applied = [c \in Clients |-> 0]
    /\ linked = [c \in Clients |-> TRUE]
    /\ delivered = [s \in Slots |-> <<>>]
    /\ seen = [c \in Clients |-> [s \in Slots |-> "absent"]]
    /\ seenEp = [c \in Clients |-> [s \in Slots |-> 0]]
    /\ restarts = 0

\* ---- The window's side: it sends, numbered ----

Keys(c) == Cardinality({i \in 1..Len(outbox[c]) : outbox[c][i].kind = "input"})

Send(c, a) ==
    /\ linked[c]
    /\ Len(outbox[c]) < MaxSends
    /\ outbox' = [outbox EXCEPT ![c] = Append(@, a)]
    /\ UNCHANGED <<epoch, term, applied, linked, delivered, seen, seenEp, restarts>>

SendCreate(c) == Send(c, [kind |-> "create", slot |-> 0, ep |-> 0, key |-> 0, size |-> 0])

\* Input only from a tab that shows the terminal running (remote_terminals.cpp sends none otherwise).
SendInput(c, s) ==
    /\ seen[c][s] = "running"
    /\ Send(c, [kind |-> "input", slot |-> s, ep |-> seenEp[c][s], key |-> Keys(c) + 1, size |-> 0])

SendResize(c, s, z) ==
    /\ seen[c][s] \in {"running", "ended"}
    /\ Send(c, [kind |-> "resize", slot |-> s, ep |-> seenEp[c][s], key |-> 0, size |-> z])

SendClose(c, s) ==
    /\ seen[c][s] \in {"running", "ended"}
    /\ Send(c, [kind |-> "close", slot |-> s, ep |-> seenEp[c][s], key |-> 0, size |-> 0])

\* ---- The server's side: TerminalBook::apply, through the Applier ----

\* A slot a create may take: never one used in this run (the code's ids within a run only rise).
Free == {s \in Slots : term[s].state = "none" \/ (term[s].state \in {"closed", "lost"} /\ term[s].ep # epoch)}

\* The effect of one action on the terminals and their input.
Effect(c, a) ==
    CASE a.kind = "create" ->
            IF Free = {} THEN UNCHANGED <<term, delivered>>     \* refused: at most kMaxTerminals
            ELSE \E s \in Free :
                    /\ term' = [term EXCEPT ![s] = [state |-> "running", ep |-> epoch, size |-> 1]]
                    /\ delivered' = [delivered EXCEPT ![s] = <<>>]
      [] a.kind = "input" ->
            IF term[a.slot].state = "running" /\ term[a.slot].ep = a.ep /\ a.ep = epoch
            THEN /\ delivered' = [delivered EXCEPT ![a.slot] = Append(@, [c |-> c, key |-> a.key, ep |-> a.ep])]
                 /\ UNCHANGED term
            ELSE UNCHANGED <<term, delivered>>                  \* refused: ended, closed or another run's
      [] a.kind = "resize" ->
            IF Live(a.slot) /\ term[a.slot].ep = a.ep
            THEN /\ term' = [term EXCEPT ![a.slot].size = a.size]
                 /\ UNCHANGED delivered
            ELSE UNCHANGED <<term, delivered>>
      [] a.kind = "close" ->
            IF Live(a.slot) /\ term[a.slot].ep = a.ep
            THEN /\ term' = [term EXCEPT ![a.slot].state = "closed"]
                 /\ UNCHANGED delivered
            ELSE UNCHANGED <<term, delivered>>

\* The next action of c's, numbered applied[c] + 1, and no other: those at or below were applied before.
Apply(c) ==
    /\ linked[c]
    /\ applied[c] < Len(outbox[c])
    /\ applied' = [applied EXCEPT ![c] = @ + 1]
    /\ Effect(c, outbox[c][applied[c] + 1])
    /\ UNCHANGED <<epoch, outbox, linked, seen, seenEp, restarts>>

\* A shell ends (it exits, or is killed): Ended, with its line.
Exit(s) ==
    /\ term[s].state = "running"
    /\ term' = [term EXCEPT ![s].state = "ended"]
    /\ UNCHANGED <<epoch, outbox, applied, linked, delivered, seen, seenEp, restarts>>

Detach(c) ==
    /\ linked[c]
    /\ linked' = [linked EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<epoch, term, outbox, applied, delivered, seen, seenEp, restarts>>

Attach(c) ==
    /\ ~linked[c]
    /\ linked' = [linked EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<epoch, term, outbox, applied, delivered, seen, seenEp, restarts>>

\* The server stops and starts again: every shell ends with it, the next run is another epoch, every link drops. The
\* store's counters stay.
Restart ==
    /\ restarts < MaxRestarts
    /\ restarts' = restarts + 1
    /\ epoch' = epoch + 1
    /\ term' = [s \in Slots |-> IF term[s].state \in {"running", "ended"} THEN [term[s] EXCEPT !.state = "lost"]
                                ELSE term[s]]
    /\ linked' = [c \in Clients |-> FALSE]
    /\ UNCHANGED <<outbox, applied, delivered, seen, seenEp>>

\* ---- The window takes the state the server sends (sync/terminal_state.h terminalFate) ----
\*
\* One tab per slot: a tab that shows a terminal lost to a restart keeps showing so until the window dismisses it, and
\* only then shows a terminal made in that slot since (the code's ids differ by epoch, so there it is a tab of its own).

\* What c's tab for s shows next, and the epoch of the terminal it shows.
SeenOf(c, s) ==
    IF seen[c][s] = "restarted" THEN "restarted"
    ELSE IF seen[c][s] \in {"running", "ended"} /\ seenEp[c][s] # epoch THEN "restarted"
    ELSE IF Live(s) THEN term[s].state
    ELSE "absent"

SeenEpOf(c, s) ==
    IF seen[c][s] = "absent" /\ Live(s) THEN epoch ELSE seenEp[c][s]

SyncWith(c, f, g) ==
    /\ linked[c]
    /\ seen' = [seen EXCEPT ![c] = [s \in Slots |-> f[s]]]
    /\ seenEp' = [seenEp EXCEPT ![c] = [s \in Slots |-> g[s]]]
    /\ seen' # seen \/ seenEp' # seenEp
    /\ UNCHANGED <<epoch, term, outbox, applied, linked, delivered, restarts>>

Sync(c) == SyncWith(c, [s \in Slots |-> SeenOf(c, s)], [s \in Slots |-> SeenEpOf(c, s)])

\* The window closes a tab that shows a terminal lost to a restart: nothing goes to the server.
Dismiss(c, s) ==
    /\ seen[c][s] = "restarted"
    /\ seen' = [seen EXCEPT ![c][s] = "absent"]
    /\ UNCHANGED <<epoch, term, outbox, applied, linked, delivered, seenEp, restarts>>

\* The actions, with the ones a reversed witness replaces as parameters.
NextWith(apply(_), detach(_), sync(_)) ==
    \/ \E c \in Clients :
        \/ SendCreate(c)
        \/ \E s \in Slots : SendInput(c, s) \/ SendClose(c, s) \/ \E z \in Sizes : SendResize(c, s, z)
        \/ apply(c)
        \/ detach(c)
        \/ Attach(c)
        \/ sync(c)
        \/ \E s \in Slots : Dismiss(c, s)
    \/ \E s \in Slots : Exit(s)
    \/ Restart

Next == NextWith(Apply, Detach, Sync)

Fairness == \A c \in Clients : WF_vars(Apply(c)) /\ WF_vars(Sync(c))

Spec == Init /\ [][Next]_vars /\ Fairness
SafeSpec == Init /\ [][Next]_vars

\* ---- Properties ----

\* Each window's keys reach a shell once each and in the order typed: strictly rising.
InputOnceInOrder ==
    \A s \in Slots : \A i, j \in 1..Len(delivered[s]) :
        (i < j /\ delivered[s][i].c = delivered[s][j].c) => delivered[s][i].key < delivered[s][j].key

\* A shell is given only input typed for its own run.
InputOnlyToItsRun ==
    \A s \in Slots : \A i \in 1..Len(delivered[s]) : delivered[s][i].ep = term[s].ep

\* Input reaches a terminal only while its shell runs: never an ended, closed or lost one.
NoInputUnlessRunning ==
    [][\A s \in Slots : Len(delivered'[s]) > Len(delivered[s]) =>
           /\ term[s].state = "running" /\ term'[s] = term[s]
           /\ Len(delivered'[s]) = Len(delivered[s]) + 1]_vars

\* A window unlinking changes no terminal: one no window shows keeps running.
DetachKeepsRunning ==
    [][(\E c \in Clients : linked[c] /\ ~linked'[c]) /\ epoch' = epoch => term' = term]_vars

\* A tab's terminal goes from the tab ("absent") only when it was closed in the run the tab knows; one lost to a restart
\* is shown so.
RestartShown ==
    [][\A c \in Clients : \A s \in Slots :
           (seen[c][s] \in {"running", "ended"} /\ seen'[c][s] = "absent") => seenEp[c][s] = epoch]_vars

\* A shell's end reaches every window that stays linked.
ExitShown ==
    \A c \in Clients : \A s \in Slots :
        (term[s].state = "ended" /\ term[s].ep = epoch /\ linked[c]) ~>
            \* "restarted": the tab still shows an earlier run's terminal of that slot (one tab per slot, above)
            (seen[c][s] \in {"ended", "restarted"} \/ term[s].state # "ended" \/ term[s].ep # epoch \/ ~linked[c])

\* A terminal lost to a restart is shown lost to a window that links again.
RestartNeverSilent ==
    \A c \in Clients : \A s \in Slots :
        (seen[c][s] \in {"running", "ended"} /\ seenEp[c][s] # epoch /\ linked[c]) ~>
            (seen[c][s] = "restarted" \/ ~linked[c])

\* Every action a linked window sent is applied.
ActionsApplied ==
    \A c \in Clients : (linked[c] /\ applied[c] < Len(outbox[c])) ~> (applied[c] = Len(outbox[c]) \/ ~linked[c])

=============================================================================

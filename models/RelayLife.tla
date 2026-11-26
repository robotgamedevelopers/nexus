------------------------------ MODULE RelayLife ------------------------------
(***************************************************************************)
(* The native window on Windows and the relays that carry its link:        *)
(* src/relay/relay_life.h (RelayLife), driven by src/relay/relay_port_win.cpp. *)
(* The window's link runs inside WSL2 as nexus-link-relay (mosh's transport *)
(* does not run natively on Windows); the window starts it with wsl.exe,   *)
(* writes it its Hello and its actions, reads its Hello and the link's     *)
(* state. A relay can end at any time (it crashed, WSL2 stopped, its bytes *)
(* were not the protocol); the window then starts another after a backoff, *)
(* and the new relay is a new link with a new client id, as a new launch   *)
(* of the window is.                                                       *)
(*                                                                         *)
(* Actions. The window numbers its actions 1, 2, ... across every relay    *)
(* (Send). Each is written to the live relay once, in order (Write); one   *)
(* not yet written when a relay ends waits for the next. The server        *)
(* applies what a relay's link delivered, per link in order, once per link *)
(* (it skips a number its client sent before: server/server.h); a relay    *)
(* that ended may still have delivered what was in flight (Apply has no    *)
(* guard on the relay being live). The live relay's state tells the window *)
(* how far the server applied that relay's actions (Ack), from its Hello   *)
(* on. When a relay ends (Death), the actions written to it and not        *)
(* acknowledged are cut: never written again, and told to the window      *)
(* (Tell) to show. The window's exit (Stop) writes nothing more and starts *)
(* no relay; the relay then ends by itself (Exit).                         *)
(*                                                                         *)
(* State:                                                                  *)
(*   phase    "wait": no relay, the next after the backoff; "hello": a     *)
(*            relay live, its Hello not read; "running": its Hello read;   *)
(*            "stopped": the window is exiting                             *)
(*   procs    relay processes running (the world's)                        *)
(*   gen      relays started; the live one is gen                          *)
(*   left     seconds left of the backoff, with "wait"                     *)
(*   backoff  the next backoff's length in seconds, unless the relay that   *)
(*            ends ran well (10 s or more: the next waits First again)      *)
(*   sent     the window's last action's number                            *)
(*   written  the last action written to a relay                           *)
(*   base     written when the live relay started: its link's k-th action  *)
(*            is the window's base + k                                     *)
(*   ackHigh  the window knows the server applied every action up to it    *)
(*            but those cut                                                *)
(*   cut      the actions cut                                              *)
(*   told     the actions cut the window was told of                       *)
(*   wr       wr[n]: the relays action n was written to (the world's)      *)
(*   ap       ap[n]: the relays whose delivery of n the server applied     *)
(*                                                                         *)
(* Properties:                                                             *)
(*   OneRelay       never two relay processes at once                      *)
(*   AppliedOnce    the server applies each action at most once            *)
(*   AckTrue        every action the window counts acknowledged, the       *)
(*                  server applied                                         *)
(*   Covered        while the window runs, every action written is         *)
(*                  acknowledged, cut, or with the live relay: none lost   *)
(*                  silently                                               *)
(*   CutOnlyUnacked a relay's end cuts only actions not acknowledged        *)
(*   BackoffOK      the backoff is First, doubling, at most Last           *)
(*   WaitsBackoff   a relay starts only when the backoff is over           *)
(*   CutTold        every action cut is told to the window (or it exits)   *)
(*   Restarts       with no relay, one starts (or the window exits, or the *)
(*                  model's relays run out)                                *)
(*                                                                         *)
(* Not modelled: the bytes and the frames (unit-tested: relay_test.cpp),   *)
(* mosh and the link inside a relay (Link.tla, Reconnect.tla), the history *)
(* requests (they carry no state).                                         *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    MaxRelays,      \* relays started, at most
    MaxActions,     \* actions the window sends, at most
    First,          \* the first backoff, in seconds (1)
    Last            \* the longest (10)

VARIABLES phase, procs, gen, left, backoff, sent, written, base, ackHigh, cut, told, wr, ap

vars == <<phase, procs, gen, left, backoff, sent, written, base, ackHigh, cut, told, wr, ap>>

Actions == 1 .. MaxActions
Min(a, b) == IF a < b THEN a ELSE b
Backoffs == {Min(First * (2 ^ k), Last) : k \in 0 .. Last}
Live == phase \in {"hello", "running"}

TypeOK ==
    /\ phase \in {"wait", "hello", "running", "stopped"}
    /\ procs \in 0 .. 2
    /\ gen \in 0 .. MaxRelays
    /\ left \in 0 .. Last
    /\ backoff \in 1 .. 2 * Last
    /\ sent \in 0 .. MaxActions
    /\ written \in 0 .. sent
    /\ base \in 0 .. written
    /\ ackHigh \in 0 .. written
    /\ cut \subseteq 1 .. written
    /\ told \subseteq cut
    /\ wr \in [Actions -> SUBSET (1 .. MaxRelays)]
    /\ ap \in [Actions -> SUBSET (1 .. MaxRelays)]

Init ==
    /\ phase = "wait"
    /\ procs = 0
    /\ gen = 0
    /\ left = 0
    /\ backoff = First
    /\ sent = 0
    /\ written = 0
    /\ base = 0
    /\ ackHigh = 0
    /\ cut = {}
    /\ told = {}
    /\ wr = [n \in Actions |-> {}]
    /\ ap = [n \in Actions |-> {}]

\* The window hands an action over: the next number, whatever the relays are doing.
Send ==
    /\ phase # "stopped"
    /\ sent < MaxActions
    /\ sent' = sent + 1
    /\ UNCHANGED <<phase, procs, gen, left, backoff, written, base, ackHigh, cut, told, wr, ap>>

\* The backoff is over and no relay runs: one starts, its link's numbers after the actions written before it.
StartWith(Ready) ==
    /\ Ready
    /\ gen < MaxRelays
    /\ phase' = "hello"
    /\ procs' = procs + 1
    /\ gen' = gen + 1
    /\ base' = written
    /\ UNCHANGED <<left, backoff, sent, written, ackHigh, cut, told, wr, ap>>
Start == StartWith(phase = "wait" /\ left = 0 /\ procs = 0)

\* The next action is written to the live relay (its Hello was written first, before it, as the port does).
Write ==
    /\ Live
    /\ written < sent
    /\ written' = written + 1
    /\ wr' = [wr EXCEPT ![written + 1] = @ \cup {gen}]
    /\ UNCHANGED <<phase, procs, gen, left, backoff, sent, base, ackHigh, cut, told, ap>>

\* The live relay's Hello is read, and is this build's.
Hello ==
    /\ phase = "hello"
    /\ phase' = "running"
    /\ UNCHANGED <<procs, gen, left, backoff, sent, written, base, ackHigh, cut, told, wr, ap>>

\* The server applies action n as relay g's link delivered it: each link's in order, each once per link.
Apply(n, g) ==
    /\ g \in wr[n]
    /\ g \notin ap[n]
    /\ \A m \in 1 .. (n - 1) : g \in wr[m] => g \in ap[m]
    /\ ap' = [ap EXCEPT ![n] = @ \cup {g}]
    /\ UNCHANGED <<phase, procs, gen, left, backoff, sent, written, base, ackHigh, cut, told, wr>>

\* The live relay's state arrives: the server applied its link's actions up to m, the window's base + k.
Ack ==
    /\ phase = "running"
    /\ \E m \in base .. written :
        /\ m >= ackHigh
        /\ \A n \in (base + 1) .. m : gen \in ap[n]
        /\ ackHigh' = m
    /\ UNCHANGED <<phase, procs, gen, left, backoff, sent, written, base, cut, told, wr, ap>>

\* What a relay's end cuts: the actions written to it and not acknowledged.
Lost == {n \in (base + 1) .. written : n > ackHigh}

\* The live relay ends, and its process has exited. The next waits First after one that ran well, else the backoff;
\* the backoff after that doubles, to at most Last.
DeathWith(Cuts, Rewind, Grow(_)) ==
    /\ Live
    /\ procs > 0
    /\ procs' = procs - 1
    /\ phase' = "wait"
    /\ \E ranWell \in BOOLEAN :
        LET wait == IF ranWell THEN First ELSE backoff IN
        /\ left' = wait
        /\ backoff' = Grow(wait)
    /\ cut' = cut \cup Cuts
    /\ written' = Rewind
    /\ UNCHANGED <<gen, sent, base, ackHigh, told, wr, ap>>
Doubled(wait) == Min(2 * wait, Last)
Death == DeathWith(Lost, written, Doubled)

\* A second of the backoff passes.
Tick ==
    /\ phase = "wait"
    /\ left > 0
    /\ left' = left - 1
    /\ UNCHANGED <<phase, procs, gen, backoff, sent, written, base, ackHigh, cut, told, wr, ap>>

\* The window's poll takes the actions cut since the last.
Tell ==
    /\ phase # "stopped"
    /\ told # cut
    /\ told' = cut
    /\ UNCHANGED <<phase, procs, gen, left, backoff, sent, written, base, ackHigh, cut, wr, ap>>

\* The window exits: nothing more is written, and no relay starts.
Stop ==
    /\ phase # "stopped"
    /\ phase' = "stopped"
    /\ left' = 0
    /\ UNCHANGED <<procs, gen, backoff, sent, written, base, ackHigh, cut, told, wr, ap>>

\* After the window's exit, the relay ends (its input closed).
Exit ==
    /\ phase = "stopped"
    /\ procs > 0
    /\ procs' = procs - 1
    /\ UNCHANGED <<phase, gen, left, backoff, sent, written, base, ackHigh, cut, told, wr, ap>>

Next ==
    \/ Send
    \/ Start
    \/ Write
    \/ Hello
    \/ \E n \in Actions, g \in 1 .. MaxRelays : Apply(n, g)
    \/ Ack
    \/ Death
    \/ Tick
    \/ Tell
    \/ Stop
    \/ Exit

\* Relays end and the window exits as they will (no fairness on Death or Stop); time passes, a relay due starts, and
\* the window polls.
Fairness == WF_vars(Tick) /\ WF_vars(Start) /\ WF_vars(Tell)

Spec == Init /\ [][Next]_vars /\ Fairness

OneRelay == procs <= 1

AppliedOnce == \A n \in Actions : Cardinality(ap[n]) <= 1

Acked == {n \in 1 .. ackHigh : n \notin cut}

AckTrue == \A n \in Acked : ap[n] # {}

Covered == phase # "stopped" => \A n \in 1 .. written : n \in Acked \/ n \in cut \/ (Live /\ n > base)

BackoffOK == backoff \in Backoffs /\ left \in 0 .. Last

CutOnlyUnacked == [][\A n \in cut' \ cut : n > ackHigh]_vars

WaitsBackoff == [][gen' # gen => (phase = "wait" /\ left = 0 /\ procs = 0)]_vars

CutTold == \A n \in Actions : (n \in cut) ~> (n \in told \/ phase = "stopped")

Restarts == (phase = "wait") ~> (phase # "wait" \/ gen = MaxRelays)

=============================================================================

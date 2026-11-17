------------------------------ MODULE Reconnect ------------------------------
(***************************************************************************)
(* The window's link making its session with nexus-server, and making it   *)
(* again when the server it reached is gone: src/link/client.h, with the   *)
(* decision in src/sync/redial.h (Redial). Link.tla is what the sessions   *)
(* carry once made, and abstracts mosh to a window that always ends up     *)
(* holding the newest state ("after a restart a window's new session       *)
(* starts from the empty state"); this module is how the window gets that  *)
(* new session when servers stop, are replaced, or crash.                  *)
(*                                                                         *)
(* Servers come and go (Start, Gone): each one started is a new server,    *)
(* numbered from 1, on the same store; Gone is a handover's exit, a stop,  *)
(* or a crash. A server that stops tells the window it ended the session   *)
(* (mosh's shutdown), which may be lost, and a crash tells nothing.        *)
(*                                                                         *)
(* The window (Dial) runs the bootstrap when it holds no session, or the   *)
(* one it holds is lost (ended by its server, or quiet: not heard for      *)
(* kQuietMs), and the backoff after its latest attempt has passed (due). A *)
(* bootstrap reaches the server running, if any: the window then holds a   *)
(* new session of it, not yet heard. With none running it fails, and the   *)
(* session held, if any, is kept. Every attempt starts a backoff (due goes *)
(* FALSE); time ends it (Time). Hear is the session's server's state       *)
(* arriving: the window has heard it, it is not quiet, and the backoff is  *)
(* over (the attempts start again from none). Time also makes a session    *)
(* quiet, whether or not its server still runs (a network that is down).   *)
(*                                                                         *)
(* State:                                                                  *)
(*   srv      the server running, 0 for none                               *)
(*   started  how many servers were started (bounds the model)             *)
(*   link     the server whose session the window holds, 0 for none        *)
(*   heard    the window has heard its session's server                    *)
(*   quiet    the session held has not been heard for kQuietMs             *)
(*   ended    its server ended it (the shutdown reached the window)        *)
(*   due      the backoff after the latest attempt has passed              *)
(*   gen      flips at each session made: a dial, seen in a trace         *)
(*                                                                         *)
(* Properties:                                                             *)
(*   DialsLive   every session the window makes is with the server        *)
(*               running then: never one of a server gone (a cached        *)
(*               CONNECT line, the old server's port and key)              *)
(*   NeverBack   once it holds a server's session, the window never holds *)
(*               one of an earlier server                                  *)
(*   Reconnects  while a server is up, the window comes to hold its       *)
(*               session and hear it: it never stays disconnected forever *)
(*               (under the fairness below)                                *)
(*                                                                         *)
(* Not modelled: the backoff's lengths (Redial's numbers, unit-tested:     *)
(* tests/sync/redial_test.cpp), the actions sent again under their own     *)
(* numbers (Link.tla's Applier, the restart test), which server a          *)
(* bootstrap that starts one would start (only before a server was heard, *)
(* at launch: Redial::mayStart).                                           *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    MaxServers  \* how many servers are started, at most

VARIABLES srv, started, link, heard, quiet, ended, due, gen

vars == <<srv, started, link, heard, quiet, ended, due, gen>>

TypeOK ==
    /\ started \in 0 .. MaxServers
    /\ srv \in 0 .. started
    /\ link \in 0 .. started
    /\ heard \in BOOLEAN
    /\ quiet \in BOOLEAN
    /\ ended \in BOOLEAN
    /\ due \in BOOLEAN
    /\ gen \in {0, 1}
    /\ link = 0 => (~heard /\ ~quiet /\ ~ended)
    \* a session ended by its server is of a server gone
    /\ ended => link # srv

Init ==
    /\ srv = 0
    /\ started = 0
    /\ link = 0
    /\ heard = FALSE
    /\ quiet = FALSE
    /\ ended = FALSE
    /\ due = TRUE
    /\ gen = 0

\* The service starts a server on the store.
Start ==
    /\ srv = 0
    /\ started < MaxServers
    /\ srv' = started + 1
    /\ started' = started + 1
    /\ UNCHANGED <<link, heard, quiet, ended, due, gen>>

\* The server goes: replaced, stopped, or crashed. One that stops may tell the window its session ended.
Gone ==
    /\ srv # 0
    /\ srv' = 0
    /\ ended' \in IF link = srv THEN {ended, TRUE} ELSE {ended}
    /\ UNCHANGED <<started, link, heard, quiet, due, gen>>

\* Redial::lost: no session, or the one held ended or quiet.
Lost == link = 0 \/ ended \/ quiet

\* The bootstrap, run when Redial::due: a new session of the server running, or a failure that keeps what was held.
DialWith(Need) ==
    /\ Need
    /\ due
    /\ due' = FALSE
    /\ IF srv # 0
       THEN /\ link' = srv
            /\ heard' = FALSE
            /\ quiet' = FALSE
            /\ ended' = FALSE
            /\ gen' = 1 - gen
       ELSE UNCHANGED <<link, heard, quiet, ended, gen>>
    /\ UNCHANGED <<srv, started>>
Dial == DialWith(Lost)

\* The session's server is heard: Redial::heard.
Hear ==
    /\ link # 0
    /\ link = srv
    /\ heard' = TRUE
    /\ quiet' = FALSE
    /\ due' = TRUE
    /\ UNCHANGED <<srv, started, link, ended, gen>>

\* Time passes: the session held may turn quiet, and the backoff may end; at least one of them.
TimeWith(Due) ==
    /\ quiet' \in IF link # 0 THEN {quiet, TRUE} ELSE {quiet}
    /\ due' \in Due
    /\ <<quiet', due'>> # <<quiet, due>>
    /\ UNCHANGED <<srv, started, link, heard, ended, gen>>
Time == TimeWith({due, TRUE})

Next ==
    \/ Start
    \/ Gone
    \/ Dial
    \/ Hear
    \/ Time

\* Servers come and go as they will (no fairness on Start or Gone). The window's attempts, its hearing a server it
\* holds a session of (mosh delivers), and time are sure to happen.
Fairness == WF_vars(Dial) /\ WF_vars(Hear) /\ WF_vars(Time)

Spec == Init /\ [][Next]_vars /\ Fairness

\* The window shows the server's live state: it holds the running server's session, has heard it, and it is not lost.
Live == link # 0 /\ link = srv /\ heard /\ ~quiet /\ ~ended

\* Every session made (gen flips) is with the server running at that moment.
DialsLive == [][gen' # gen => (srv # 0 /\ link' = srv)]_vars

\* The window never goes back to an earlier server's session.
NeverBack == [][link' >= link]_vars

\* While a server is up, the window comes to show its live state (or the server goes first).
Reconnects == (srv # 0) ~> (srv = 0 \/ Live)

=============================================================================

------------------------------- MODULE Relay -------------------------------
(***************************************************************************)
(* The watch's prompts to nexus-server through the phone (watch/core/relay.h): *)
(* the watch's outbox (WatchLink), frames to the phone and answers back     *)
(* over a carrier that loses, repeats and reorders them, the phone's relay  *)
(* (PhoneRelay) with the one number it keeps across restarts (`handed`),    *)
(* and the phone's link to the server.                                      *)
(*                                                                         *)
(* The link is abstract: within one run of the phone it delivers each      *)
(* action handed to it once (models/Link.tla and Reconnect.tla check that), *)
(* in any order here, and acknowledges them all once all are delivered     *)
(* (link::ClientLink's snapshot: `acknowledged` is all or nothing). A phone *)
(* that restarts loses what it held in memory; what its old link had sent  *)
(* may still arrive at the server, or never.                               *)
(*                                                                         *)
(* What it must keep: no prompt applied twice, none dropped from the        *)
(* watch's outbox unless the server has it or the person was told it may   *)
(* not have arrived, and, once the phone stops restarting, every prompt    *)
(* leaves the outbox.                                                      *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    N,          \* prompts the watch makes
    MaxCrash,   \* phone restarts
    MaxMsgs     \* frames, and answers, in flight at once

VARIABLES
    made,       \* prompts made: 1..made
    outbox,     \* the watch's outbox
    told,       \* prompts the watch told the person may not have arrived
    dropped,    \* prompts the watch dropped as applied
    reqs,       \* frames on their way to the phone: each the outbox when sent
    resps,      \* answers on their way to the watch: [uncertain, applied]
    handed,     \* the phone's durable number: the highest prompt ever handed to a link
    boot,       \* `handed` when this run of the phone started
    acked,      \* this run: prompts up to here are done with
    waiting,    \* this run: prompts received and not yet handed, in order
    inflight,   \* this run: handed and not yet acknowledged, in order
    linkQ,      \* actions handed to a link (any run's) and not yet delivered
    applied,    \* how many times the server applied each prompt
    crashes

vars == <<made, outbox, told, dropped, reqs, resps, handed, boot, acked, waiting, inflight, linkQ, applied, crashes>>
phoneVars == <<handed, boot, acked, waiting, inflight>>

Range(s) == {s[i] : i \in 1..Len(s)}
Max(S) == CHOOSE m \in S : \A n \in S : n <= m
\* The numbers lo..hi as a sequence.
Seq2(lo, hi) == [i \in 1..(hi - lo + 1) |-> lo + i - 1]

Init ==
    /\ made = 0 /\ outbox = {} /\ told = {} /\ dropped = {}
    /\ reqs = {} /\ resps = {}
    /\ handed = 0 /\ boot = 0 /\ acked = 0 /\ waiting = << >> /\ inflight = << >>
    /\ linkQ = {} /\ applied = [n \in 1..N |-> 0] /\ crashes = 0

\* The person says something: the next prompt, in the outbox.
Make ==
    /\ made < N
    /\ made' = made + 1 /\ outbox' = outbox \cup {made + 1}
    /\ UNCHANGED <<told, dropped, reqs, resps, phoneVars, linkQ, applied, crashes>>

\* The watch sends a frame: its outbox as it is.
Send ==
    /\ Cardinality(reqs) < MaxMsgs
    /\ reqs' = reqs \cup {outbox}
    /\ UNCHANGED <<made, outbox, told, dropped, resps, phoneVars, linkQ, applied, crashes>>

\* What a frame adds to the phone's waiting prompts: the run of numbers it holds from the next one expected.
Taken(r) ==
    LET next == handed + Len(waiting) + 1
        tops == {m \in next..N : \A k \in next..m : k \in r}
    IN IF tops = {} THEN << >> ELSE Seq2(next, Max(tops))

\* The phone's answer: what it cannot vouch for (its run's start) and what is done with.
Answer == [uncertain |-> boot, applied |-> acked]

\* The phone takes a frame and answers it (PhoneRelay::exchange).
Receive(r) ==
    /\ waiting' = waiting \o Taken(r)
    /\ resps' = IF Cardinality(resps) < MaxMsgs THEN resps \cup {Answer} ELSE resps
    /\ UNCHANGED <<made, outbox, told, dropped, reqs, handed, boot, acked, inflight, linkQ, applied, crashes>>

\* The phone hands what waits to the link, noting `handed` durably first (PhoneRelay::link).
Hand ==
    /\ waiting # << >>
    /\ handed' = waiting[Len(waiting)]
    /\ inflight' = inflight \o waiting /\ linkQ' = linkQ \cup Range(waiting) /\ waiting' = << >>
    /\ UNCHANGED <<made, outbox, told, dropped, reqs, resps, boot, acked, applied, crashes>>

\* The server applies an action the link delivered.
Deliver(n) ==
    /\ n \in linkQ
    /\ linkQ' = linkQ \ {n} /\ applied' = [applied EXCEPT ![n] = @ + 1]
    /\ UNCHANGED <<made, outbox, told, dropped, reqs, resps, phoneVars, crashes>>

\* The link reports every action of this run acknowledged: the relay is done with all it handed.
Ack ==
    /\ inflight # << >>
    /\ \A i \in 1..Len(inflight) : inflight[i] \notin linkQ
    /\ acked' = Max({acked} \cup Range(inflight)) /\ inflight' = << >>
    /\ UNCHANGED <<made, outbox, told, dropped, reqs, resps, handed, boot, waiting, linkQ, applied, crashes>>

\* The watch takes an answer (WatchLink::take): what the phone cannot vouch for is told, what is done with goes.
Take(m) ==
    LET doubt == {n \in outbox : n <= m.uncertain}
        done == {n \in outbox : m.uncertain < n /\ n <= m.applied}
    IN /\ outbox' = outbox \ (doubt \cup done)
       /\ told' = told \cup doubt /\ dropped' = dropped \cup done
       /\ UNCHANGED <<made, reqs, resps, phoneVars, linkQ, applied, crashes>>

\* A whole exchange at once, the watch's frame and the answer to it: the carrier working, which fairness assumes.
RoundTrip ==
    LET w2 == waiting \o Taken(outbox)
        doubt == {n \in outbox : n <= boot}
        done == {n \in outbox : boot < n /\ n <= acked}
    IN /\ waiting' = w2
       /\ outbox' = outbox \ (doubt \cup done) /\ told' = told \cup doubt /\ dropped' = dropped \cup done
       /\ UNCHANGED <<made, reqs, resps, handed, boot, acked, inflight, linkQ, applied, crashes>>

\* The phone restarts: all but `handed` is gone; its old link's actions may still arrive, or never.
Crash ==
    /\ crashes < MaxCrash
    /\ crashes' = crashes + 1
    /\ boot' = handed /\ acked' = handed /\ waiting' = << >> /\ inflight' = << >>
    /\ UNCHANGED <<made, outbox, told, dropped, reqs, resps, handed, linkQ, applied>>

LoseReq(r) == reqs' = reqs \ {r} /\ UNCHANGED <<made, outbox, told, dropped, resps, phoneVars, linkQ, applied, crashes>>
LoseResp(m) == resps' = resps \ {m} /\ UNCHANGED <<made, outbox, told, dropped, reqs, phoneVars, linkQ, applied, crashes>>

Next ==
    \/ Make \/ Send \/ Hand \/ Ack \/ Crash \/ RoundTrip
    \/ \E r \in reqs : Receive(r) \/ LoseReq(r)
    \/ \E m \in resps : Take(m) \/ LoseResp(m)
    \/ \E n \in 1..N : Deliver(n)

Spec == Init /\ [][Next]_vars
\* The carrier works now and then, the phone hands what waits, its link delivers what this run handed and says so.
\* Actions an earlier run's link held may never arrive: no fairness for them.
DeliverCurrent == \E i \in 1..Len(inflight) : Deliver(inflight[i])
FairSpec == Spec /\ WF_vars(RoundTrip) /\ WF_vars(Hand) /\ WF_vars(DeliverCurrent) /\ WF_vars(Ack)

-----------------------------------------------------------------------------
TypeOK ==
    /\ made \in 0..N /\ outbox \subseteq 1..made /\ told \subseteq 1..made /\ dropped \subseteq 1..made
    /\ handed \in 0..N /\ boot \in 0..handed /\ acked \in 0..N
    /\ Range(waiting) \subseteq 1..N /\ Range(inflight) \subseteq 1..N /\ linkQ \subseteq 1..N
    /\ applied \in [1..N -> Nat] /\ crashes \in 0..MaxCrash

\* Nothing applied twice.
AtMostOnce == \A n \in 1..N : applied[n] <= 1
\* Nothing dropped silently: every prompt made is still held, or the server has it, or the person was told.
NoSilentLoss == \A n \in 1..made : n \in outbox \/ applied[n] = 1 \/ n \in told
\* A prompt dropped as applied was applied.
DroppedWasApplied == \A n \in dropped : applied[n] = 1
\* The phone never hands a prompt it handed before, in any run.
HandedOnce == \A n \in 1..N : Cardinality({i \in 1..Len(inflight) : inflight[i] = n}) <= 1
\* Once the phone stops restarting, every prompt leaves the outbox.
Delivered == \A n \in 1..N : (n \in outbox) ~> (n \notin outbox)
=============================================================================

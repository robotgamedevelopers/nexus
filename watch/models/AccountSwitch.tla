---------------------------- MODULE AccountSwitch ----------------------------
(***************************************************************************)
(* Switching an AI's account from the watch (watch/core/relay.h: WatchLink  *)
(* choose and take, PhoneRelay exchange, link and answer), beside Relay.tla *)
(* and over the same carrier: frames and answers lost, repeated and         *)
(* reordered, a phone that restarts and forgets all it held, and a link     *)
(* that delivers what a run handed and acknowledges all of it at once.      *)
(*                                                                         *)
(* One AI. The watch shows the account the phone last said the AI is on    *)
(* (`shown`) and lists the signed-in accounts; the person picks one. The    *)
(* watch numbers its choice and holds the newest until an answer says the   *)
(* phone is done with it (`switched`); the phone hands the newest it has    *)
(* taken to the link as a SetAccount on the AI's session, and is done with  *)
(* it once the link acknowledges it. Nothing about a switch is durable on   *)
(* the phone: a restarted phone takes the held choice again and hands it    *)
(* again, which sets the same account again.                               *)
(*                                                                         *)
(* What it must keep: only a signed-in account is ever chosen or handed; a  *)
(* choice of the account the AI is on (or of the one held) is never made;   *)
(* the watch lets go of a choice unapplied only when the person made a      *)
(* newer one; and, once the phone stops restarting, every choice leaves     *)
(* the watch.                                                              *)
(*                                                                         *)
(* Not kept, and not claimed: that the server ends on the newest choice. An *)
(* earlier run's link may still deliver an older SetAccount after a newer   *)
(* one (Relay.tla's link, the same here), and nothing on the wire orders    *)
(* them. The watch shows what the server says, never its own wish.          *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Accounts,   \* the AI's accounts
    SignedIn,   \* those signed in: the only ones the watch lists
    Start,      \* the account the AI's session is on at first
    MaxChoice,  \* choices the person makes
    MaxCrash,   \* phone restarts
    MaxMsgs     \* frames, and answers, in flight at once

None == [num |-> 0, acct |-> ""]
Choices == [num : 0..MaxChoice, acct : Accounts \cup {""}]

VARIABLES
    server,     \* the account the AI's session runs on, at the server
    applied,    \* choice numbers the server applied (a SetAccount delivered)
    chosen,     \* the choices made: [num, acct]
    choice,     \* the watch's held choice, or None
    shown,      \* the AI's account as the last answer the watch took said; "" before any, or none sent
    reqs,       \* frames on their way to the phone: the choice each carried
    resps,      \* answers on their way to the watch: [switched, current]
    seen,       \* the phone, this run: the newest choice number taken
    waiting,    \* the phone, this run: the choice taken and not yet handed, or None
    inflight,   \* the phone, this run: the newest choice handed and not yet acknowledged, or None
    runQ,       \* the phone, this run: what this run handed (the link acknowledges once all is delivered)
    switched,   \* the phone, this run: choices up to here are done with
    view,       \* the phone: the server's account as its link last showed it; "" in a new run before any
    linkQ,      \* SetAccounts handed to a link (any run's) and not yet delivered
    crashes

vars == <<server, applied, chosen, choice, shown, reqs, resps, seen, waiting, inflight, runQ, switched, view, linkQ,
          crashes>>
phoneVars == <<seen, waiting, inflight, runQ, switched, view>>

Max(a, b) == IF a >= b THEN a ELSE b

Init ==
    /\ server = Start /\ applied = {} /\ chosen = {} /\ choice = None /\ shown = ""
    /\ reqs = {} /\ resps = {}
    /\ seen = 0 /\ waiting = None /\ inflight = None /\ runQ = {} /\ switched = 0 /\ view = ""
    /\ linkQ = {} /\ crashes = 0

\* What the watch's choice would change from: the one it holds, else the AI's account as shown.
Target == IF choice # None THEN choice.acct ELSE shown

\* The person picks a signed-in account from the list (WatchLink::choose); a pick of the target sends nothing.
Choose(a) ==
    /\ Cardinality(chosen) < MaxChoice
    /\ shown # ""
    /\ a \in SignedIn
    /\ a # Target
    /\ choice' = [num |-> Cardinality(chosen) + 1, acct |-> a]
    /\ chosen' = chosen \cup {choice'}
    /\ UNCHANGED <<server, applied, shown, reqs, resps, phoneVars, linkQ, crashes>>

\* The watch sends a frame, its held choice in it.
Send ==
    /\ Cardinality(reqs) < MaxMsgs
    /\ reqs' = reqs \cup {choice}
    /\ UNCHANGED <<server, applied, chosen, choice, shown, resps, phoneVars, linkQ, crashes>>

Answer == [switched |-> switched, current |-> view]

\* The phone takes a frame (PhoneRelay::exchange): a newer choice than any this run took replaces the waiting one.
TakeChoice(r) ==
    IF r.num > seen THEN seen' = r.num /\ waiting' = r ELSE UNCHANGED <<seen, waiting>>

Receive(r) ==
    /\ TakeChoice(r)
    /\ resps' = IF Cardinality(resps) < MaxMsgs THEN resps \cup {Answer} ELSE resps
    /\ UNCHANGED <<server, applied, chosen, choice, shown, reqs, inflight, runQ, switched, view, linkQ, crashes>>

\* The phone moves the link's latest state in (PhoneRelay::link) and hands the waiting choice, if any, as a SetAccount.
Hand ==
    /\ waiting # None
    /\ view' = server
    /\ inflight' = waiting /\ runQ' = runQ \cup {waiting} /\ linkQ' = linkQ \cup {waiting} /\ waiting' = None
    /\ UNCHANGED <<server, applied, chosen, choice, shown, reqs, resps, seen, switched, crashes>>

\* The same with nothing to hand: the phone's view of the server catches up.
Look ==
    /\ view' = server
    /\ UNCHANGED <<server, applied, chosen, choice, shown, reqs, resps, seen, waiting, inflight, runQ, switched, linkQ,
                   crashes>>

\* The server applies a SetAccount a link delivered.
Deliver(x) ==
    /\ x \in linkQ
    /\ linkQ' = linkQ \ {x} /\ server' = x.acct /\ applied' = applied \cup {x.num}
    /\ UNCHANGED <<chosen, choice, shown, reqs, resps, phoneVars, crashes>>

\* The link says all this run handed is acknowledged: the phone is done with the newest it handed.
Ack ==
    /\ inflight # None
    /\ runQ \cap linkQ = {}
    /\ switched' = Max(switched, inflight.num) /\ inflight' = None /\ view' = server
    /\ UNCHANGED <<server, applied, chosen, choice, shown, reqs, resps, seen, waiting, runQ, linkQ, crashes>>

\* The watch takes an answer (WatchLink::take): a choice the phone is done with goes; the account shown is the answer's.
TakeAnswer(m) ==
    /\ choice' = IF choice # None /\ choice.num <= m.switched THEN None ELSE choice
    /\ shown' = m.current

Take(m) ==
    /\ TakeAnswer(m)
    /\ UNCHANGED <<server, applied, chosen, reqs, resps, phoneVars, linkQ, crashes>>

\* A whole exchange at once: the carrier working, which fairness assumes.
RoundTrip ==
    /\ TakeChoice(choice)
    /\ LET m == [switched |-> switched, current |-> view] IN TakeAnswer(m)
    /\ UNCHANGED <<server, applied, chosen, reqs, resps, inflight, runQ, switched, view, linkQ, crashes>>

\* The phone restarts: all it held about switches is gone; its old link's SetAccounts may still arrive, or never.
Crash ==
    /\ crashes < MaxCrash
    /\ crashes' = crashes + 1
    /\ seen' = 0 /\ waiting' = None /\ inflight' = None /\ runQ' = {} /\ switched' = 0 /\ view' = ""
    /\ UNCHANGED <<server, applied, chosen, choice, shown, reqs, resps, linkQ>>

LoseReq(r) == reqs' = reqs \ {r} /\ UNCHANGED <<server, applied, chosen, choice, shown, resps, phoneVars, linkQ, crashes>>
LoseResp(m) == resps' = resps \ {m} /\ UNCHANGED <<server, applied, chosen, choice, shown, reqs, phoneVars, linkQ, crashes>>

Next ==
    \/ \E a \in Accounts : Choose(a)
    \/ Send \/ Hand \/ Look \/ Ack \/ Crash \/ RoundTrip
    \/ \E r \in reqs : Receive(r) \/ LoseReq(r)
    \/ \E m \in resps : Take(m) \/ LoseResp(m)
    \/ \E x \in linkQ : Deliver(x)

Spec == Init /\ [][Next]_vars
\* The carrier works now and then, the phone hands what waits, this run's link delivers what it handed and says so.
DeliverCurrent == \E x \in runQ : Deliver(x)
FairSpec == Spec /\ WF_vars(RoundTrip) /\ WF_vars(Hand) /\ WF_vars(Look) /\ WF_vars(DeliverCurrent) /\ WF_vars(Ack)

-----------------------------------------------------------------------------
TypeOK ==
    /\ server \in Accounts /\ applied \subseteq 1..MaxChoice /\ chosen \subseteq Choices /\ choice \in Choices
    /\ shown \in Accounts \cup {""} /\ reqs \subseteq Choices
    /\ resps \subseteq [switched : 0..MaxChoice, current : Accounts \cup {""}]
    /\ seen \in 0..MaxChoice /\ waiting \in Choices /\ inflight \in Choices /\ runQ \subseteq Choices
    /\ switched \in 0..MaxChoice /\ view \in Accounts \cup {""} /\ linkQ \subseteq Choices /\ crashes \in 0..MaxCrash

\* Only a signed-in account is ever chosen, or handed to a link.
OnlySignedIn == \A c \in chosen \cup linkQ : c.acct \in SignedIn
\* The watch lets go of a choice unapplied only for a newer one the person made.
NoSilentDrop == \A c \in chosen : c = choice \/ c.num \in applied \/ \E d \in chosen : d.num > c.num
\* A choice always changes something: never the account the AI is shown on, nor the one already held.
ChoiceChanges == [][\A c \in chosen' \ chosen : c.acct # Target]_vars
\* Once the phone stops restarting, every choice leaves the watch.
ChoiceLeaves == \A k \in 1..MaxChoice : (choice.num = k) ~> (choice.num # k)
=============================================================================

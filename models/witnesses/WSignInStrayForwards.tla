--------------------------- MODULE WSignInStrayForwards ---------------------------
\* Reversed witness: a window's gate that sends up the first request it gets, whatever its path or state (a favicon, a
\* stray tab, a page of another flow). Claude Code's listener aborts the whole login on a wrong one, so the gate must
\* send only the matching callback: GateForwardsOnlyMatching must reject BrokenSpec.
\*
\* With the same gate the server's own check still holds (HostileSpec, WSignInStrayHostile.cfg, which must pass): it
\* replays only the callback whose state matches, once. ReplayAny drops that check too (ReplayAnySpec,
\* WSignInReplayAny.cfg): CallbackOnlyMatching must reject it.
EXTENDS MCSignIn

StrayForwards(c) ==
    /\ up' = up \cup {[kind |-> "callback", id |-> gate[c], c |-> c, match |-> FALSE, ok |-> TRUE]}
    /\ gate' = [gate EXCEPT ![c] = 0]

\* The server replays every callback for a running flow, whoever sent it and whatever its state.
ReplayAny(m) ==
    LET i == m.id
        a == flow[i].acct
    IN IF flow[i].st \in Running
       THEN /\ flow' = [flow EXCEPT ![i].st = "callback-forwarded"]
            /\ cbs' = [cbs EXCEPT ![i] = @ + 1]
            /\ badcb' = IF m.match THEN badcb ELSE badcb \cup {i}
            /\ proc' = IF proc[a].id = i /\ proc[a].st = "url"
                       THEN [proc EXCEPT ![a].st = IF m.ok THEN "got-code" ELSE "got-denied"]
                       ELSE proc
            /\ log' = {[id |-> i, event |-> "forwarded"]}
       ELSE CallbackCorrect(m)

BrokenSpec == Init /\ [][ServerNextWith(Need, Receive, Print, Timeout) \/
                         WindowNextWith(Redirect, LAMBDA c : StrayWith(StrayForwards, c))]_vars
HostileSpec == BrokenSpec
ReplayAnySpec == Init /\ [][ServerNextWith(Need, LAMBDA m : ReceiveWith(ClaimCorrect, OpenedCorrect, ReplayAny, m),
                                           Print, Timeout) \/
                            WindowNextWith(Redirect, LAMBDA c : StrayWith(StrayForwards, c))]_vars
=============================================================================

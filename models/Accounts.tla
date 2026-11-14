------------------------------ MODULE Accounts ------------------------------
(***************************************************************************)
(* The accounts and friends nexus-server holds (src/accounts/registry.h,   *)
(* src/sync/hub.h): the providers' accounts, the friends, the links from a *)
(* friend to the accounts it may run on (in order, with one of them its    *)
(* default, or none), and which account each session runs on.             *)
(*                                                                         *)
(* An account is set up on its own, with its provider, and starts needing  *)
(* sign-in; what the sign-in later reports (signed in, or needing it       *)
(* again) is the server's own step `Report`. A friend links to any number  *)
(* of accounts, of any providers, and an account serves any number of      *)
(* friends. A session is made with a friend (`Create`), on the account of  *)
(* the session it copies when that account is linked to its friend, else  *)
(* on the friend's default. The person sets a session's account to one     *)
(* linked to its friend, or to none (`SetSession`); its harness becomes    *)
(* the account's provider, so a switch may cross providers. The server     *)
(* rotates a session to its friend's next signed-in linked account, in     *)
(* link order, when its account reports a usage limit (`Rotate`).          *)
(*                                                                         *)
(* Unlinking an account from a friend, or removing it, moves every session *)
(* on it off it, in the same step, to the friend's default when that is    *)
(* signed in, else to the friend's first signed-in linked account in link  *)
(* order after it, else to none; and each session moved is told (a notice  *)
(* in its transcript). An unlinked or removed default becomes none.        *)
(*                                                                         *)
(* `told` and `last` are history: the sessions given a notice by the step  *)
(* just taken, and the operation with its arguments and result. No action  *)
(* reads either, so TLC tells states apart without them (VIEW).            *)
(*                                                                         *)
(* Not modelled: what a change of a session's account does to its driver  *)
(* (Supervisor.tla), the sessions' transcripts and queues (Link.tla),      *)
(* the store's durability (models/store), and names' and limits' checks    *)
(* (unit-tested: accounts_unit, hub_accounts).                             *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Accts,          \* the account names an instance uses
    Provider,       \* each account's provider (the harness its sessions run)
    Friends,        \* the friend names an instance uses
    MaxSes,         \* the sessions an instance may make (ids 1 .. MaxSes)
    StartHarness    \* the harness of a session on no account (the friend's harness)

None == "none"
Ids == 1..MaxSes
Range(sq) == {sq[i] : i \in DOMAIN sq}
Providers == {Provider[a] : a \in Accts} \cup {StartHarness}
ASSUME None \notin Accts \cup Friends \cup Providers

VARIABLES
    acct,       \* each account: "absent", "in" (signed in) or "out" (needs sign-in)
    known,      \* the friends set up
    links,      \* each friend's linked accounts, in link order
    dflt,       \* each friend's default account, or None
    ses,        \* each session id: made, its friend, its account (or None) and its harness
    nextSes,    \* the id the next session made gets
    told,       \* history: the sessions the step gave a notice
    last        \* history: the operation, its arguments and its result

model == <<acct, known, links, dflt, ses, nextSes>>
vars == <<acct, known, links, dflt, ses, nextSes, told, last>>
View == model

NoSes == [made |-> FALSE, friend |-> None, acct |-> None, harness |-> None]

Up(a) == acct[a] = "in"
Drop(sq, x) == SelectSeq(sq, LAMBDA y : y /= x)
Pos(sq, x) == IF x \in Range(sq) THEN CHOOSE i \in DOMAIN sq : sq[i] = x ELSE 0
\* The accounts after x in sq, in order, cycling round; x itself never (all of sq when x is not in it).
After(sq, x) == LET i == Pos(sq, x) IN
    IF i = 0 THEN sq ELSE SubSeq(sq, i + 1, Len(sq)) \o SubSeq(sq, 1, i - 1)
\* The friend's next signed-in linked account after x, in link order, or None.
NextUp(f, x) == LET c == SelectSeq(After(links[f], x), Up) IN IF c = << >> THEN None ELSE Head(c)
\* Where a session of friend f on x goes when x leaves the friend: its default when signed in, else the next.
Fallback(f, x) == IF dflt[f] \notin {x, None} /\ Up(dflt[f]) THEN dflt[f] ELSE NextUp(f, x)
HarnessFor(a, h) == IF a = None THEN h ELSE Provider[a]
On(x) == {s \in Ids : ses[s].made /\ ses[s].acct = x}
MoveOff(x, S) == [s \in Ids |-> IF s \in S
                                THEN LET n == Fallback(ses[s].friend, x) IN
                                     [ses[s] EXCEPT !.acct = n, !.harness = HarnessFor(n, @)]
                                ELSE ses[s]]

Done(op, f, a, s, ok) == last' = [op |-> op, f |-> f, a |-> a, s |-> s, ok |-> ok]
Refused(op, f, a, s, notice) ==
    /\ Done(op, f, a, s, FALSE)
    /\ told' = notice
    /\ UNCHANGED model

Init ==
    /\ acct = [a \in Accts |-> "absent"]
    /\ known = {}
    /\ links = [f \in Friends |-> << >>]
    /\ dflt = [f \in Friends |-> None]
    /\ ses = [s \in Ids |-> NoSes]
    /\ nextSes = 1
    /\ told = {}
    /\ last = [op |-> "init", f |-> None, a |-> None, s |-> 0, ok |-> TRUE]

\* The person's: an account set up, with its provider; it needs sign-in until a sign-in reports otherwise.
AddAccount(a) ==
    IF acct[a] = "absent"
    THEN /\ acct' = [acct EXCEPT ![a] = "out"]
         /\ Done("add", None, a, 0, TRUE) /\ told' = {}
         /\ UNCHANGED <<known, links, dflt, ses, nextSes>>
    ELSE Refused("add", None, a, 0, {})

\* The server's: a sign-in (or its loss) reported for an account.
Report(a, st) ==
    IF acct[a] /= "absent"
    THEN /\ acct' = [acct EXCEPT ![a] = st]
         /\ Done(IF st = "in" THEN "in" ELSE "out", None, a, 0, TRUE) /\ told' = {}
         /\ UNCHANGED <<known, links, dflt, ses, nextSes>>
    ELSE Refused(IF st = "in" THEN "in" ELSE "out", None, a, 0, {})

\* The person's: an account removed. Every friend's link to it goes, a default on it becomes none, and every session
\* on it moves off it and is told.
RemoveAccount(a) ==
    IF acct[a] /= "absent"
    THEN LET S == On(a) IN
         /\ acct' = [acct EXCEPT ![a] = "absent"]
         /\ links' = [f \in Friends |-> Drop(links[f], a)]
         /\ dflt' = [f \in Friends |-> IF dflt[f] = a THEN None ELSE dflt[f]]
         /\ ses' = MoveOff(a, S)
         /\ told' = S
         /\ Done("remove", None, a, 0, TRUE)
         /\ UNCHANGED <<known, nextSes>>
    ELSE Refused("remove", None, a, 0, {})

AddFriend(f) ==
    IF f \notin known
    THEN /\ known' = known \cup {f}
         /\ Done("friend", f, None, 0, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, links, dflt, ses, nextSes>>
    ELSE Refused("friend", f, None, 0, {})

LinkAccount(f, a) ==
    IF f \in known /\ acct[a] /= "absent" /\ a \notin Range(links[f])
    THEN /\ links' = [links EXCEPT ![f] = Append(@, a)]
         /\ Done("link", f, a, 0, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, known, dflt, ses, nextSes>>
    ELSE Refused("link", f, a, 0, {})

\* The person's: the friend no longer runs on the account. Its sessions on it move off it and are told.
Unlink(f, a) ==
    IF a \in Range(links[f])
    THEN LET S == {s \in On(a) : ses[s].friend = f} IN
         /\ links' = [links EXCEPT ![f] = Drop(@, a)]
         /\ dflt' = [dflt EXCEPT ![f] = IF @ = a THEN None ELSE @]
         /\ ses' = MoveOff(a, S)
         /\ told' = S
         /\ Done("unlink", f, a, 0, TRUE)
         /\ UNCHANGED <<acct, known, nextSes>>
    ELSE Refused("unlink", f, a, 0, {})

SetDefault(f, a) ==
    IF f \in known /\ (a = None \/ a \in Range(links[f]))
    THEN /\ dflt' = [dflt EXCEPT ![f] = a]
         /\ Done("default", f, a, 0, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, known, links, ses, nextSes>>
    ELSE Refused("default", f, a, 0, {})

\* The person's: the session's account, one linked to its friend or none, whatever its provider or sign-in. A
\* refusal is a notice in the session's transcript.
SetSession(s, a) ==
    IF ses[s].made /\ (a = None \/ a \in Range(links[ses[s].friend]))
    THEN /\ ses' = [ses EXCEPT ![s].acct = a, ![s].harness = HarnessFor(a, @)]
         /\ Done("set", None, a, s, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, known, links, dflt, nextSes>>
    ELSE Refused("set", None, a, s, IF ses[s].made THEN {s} ELSE {})

\* The server's, where a usage limit is reported: the friend's next signed-in linked account after the session's, in
\* link order. The session is told either way: of the switch, or that there is none to switch to.
Rotate(s) ==
    IF ses[s].made
    THEN LET n == NextUp(ses[s].friend, ses[s].acct) IN
         IF n = None
         THEN Refused("rotate", None, None, s, {s})
         ELSE /\ ses' = [ses EXCEPT ![s].acct = n, ![s].harness = Provider[n]]
              /\ Done("rotate", None, n, s, TRUE) /\ told' = {s}
              /\ UNCHANGED <<acct, known, links, dflt, nextSes>>
    ELSE Refused("rotate", None, None, s, {})

\* A session made with friend f, copying session `from` (0: none): its account when linked to f (or none), else
\* f's default. Past the limit, or copying a session not made, refused.
Create(f, from) ==
    IF nextSes <= MaxSes /\ (from = 0 \/ ses[from].made)
    THEN LET a == IF from /= 0 /\ (ses[from].acct = None \/ ses[from].acct \in Range(links[f]))
                  THEN ses[from].acct ELSE dflt[f] IN
         /\ ses' = [ses EXCEPT ![nextSes] = [made |-> TRUE, friend |-> f, acct |-> a,
                                             harness |-> HarnessFor(a, StartHarness)]]
         /\ nextSes' = nextSes + 1
         /\ Done("create", f, a, from, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, known, links, dflt>>
    ELSE Refused("create", f, None, from, {})

\* The steps, with the actions a reversed witness breaks passed in.
NextWith(Add(_), Rm(_), Ul(_, _), Def(_, _), St(_, _), Rot(_), Cr(_, _)) ==
    \/ \E a \in Accts : Add(a) \/ Rm(a) \/ Report(a, "in") \/ Report(a, "out")
    \/ \E f \in Friends : AddFriend(f)
    \/ \E f \in Friends, a \in Accts : LinkAccount(f, a) \/ Ul(f, a)
    \/ \E f \in Friends, a \in Accts \cup {None} : Def(f, a)
    \/ \E s \in Ids, a \in Accts \cup {None} : St(s, a)
    \/ \E s \in Ids : Rot(s)
    \/ \E f \in Friends, from \in 0..MaxSes : Cr(f, from)

Next == NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, Rotate, Create)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* Invariants.

Session == [made : BOOLEAN, friend : Friends \cup {None}, acct : Accts \cup {None}, harness : Providers \cup {None}]

TypeOK ==
    /\ acct \in [Accts -> {"absent", "in", "out"}]
    /\ known \subseteq Friends
    /\ \A f \in Friends : links[f] \in Seq(Accts)
    /\ dflt \in [Friends -> Accts \cup {None}]
    /\ ses \in [Ids -> Session]
    /\ nextSes \in 1..(MaxSes + 1)
    /\ told \subseteq Ids
    /\ last.ok \in BOOLEAN

\* Links name accounts set up, each once, only from friends set up.
LinksSound ==
    \A f \in Friends :
        /\ \A i \in DOMAIN links[f] : acct[links[f][i]] /= "absent"
        /\ Len(links[f]) = Cardinality(Range(links[f]))
        /\ f \notin known => links[f] = << >>

\* A friend's default is one of its linked accounts, or none.
DefaultLinked == \A f \in Friends : dflt[f] = None \/ dflt[f] \in Range(links[f])

\* A session runs on an account linked to its friend, or on none: never on one unlinked or removed.
SessionLinked ==
    \A s \in Ids : ses[s].made => (ses[s].acct = None \/ ses[s].acct \in Range(links[ses[s].friend]))

\* A session on an account runs its provider's harness.
HarnessMatches == \A s \in Ids : ses[s].made /\ ses[s].acct /= None => ses[s].harness = Provider[ses[s].acct]

\* The sessions made are exactly the ids below the next.
IdsDense == \A s \in Ids : ses[s].made <=> s < nextSes

-----------------------------------------------------------------------------
\* Action properties, on the step's history.

\* A session's account changes without a notice to it only when the person set it.
MovedIsTold ==
    [][\A s \in Ids : ses[s].made /\ ses'[s].acct /= ses[s].acct /\ ~(last'.op = "set" /\ last'.s = s)
                        => s \in told']_vars

\* An account the server picks (a rotation, or a move off an account unlinked or removed) is signed in.
ServerPicksSignedIn ==
    [][\A s \in Ids : ses[s].made /\ ses'[s].acct /= ses[s].acct /\ ses'[s].acct /= None
                      /\ last'.op \in {"rotate", "remove", "unlink"}
                        => acct'[ses'[s].acct] = "in"]_vars

\* A rotation moves the session whenever its friend has another signed-in linked account.
RotateMoves ==
    [][last'.op = "rotate" /\ ses[last'.s].made
       /\ (\E b \in Range(links[ses[last'.s].friend]) : b /= ses[last'.s].acct /\ acct[b] = "in")
         => ses'[last'.s].acct /= ses[last'.s].acct]_vars

\* A set is taken exactly for a session made and an account linked to its friend (or none), and gives it that one.
SetExact ==
    [][last'.op = "set" =>
         /\ last'.ok <=> (ses[last'.s].made /\ (last'.a = None \/ last'.a \in Range(links[ses[last'.s].friend])))
         /\ last'.ok => ses'[last'.s].acct = last'.a]_vars

\* A set or a rotation changes no other session.
OnlyItsSession == [][last'.op \in {"set", "rotate"} => \A s \in Ids \ {last'.s} : ses'[s] = ses[s]]_vars

\* An operation refused changes nothing.
RefusedChangesNothing == [][~last'.ok => UNCHANGED model]_vars

=============================================================================

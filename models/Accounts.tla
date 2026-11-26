------------------------------ MODULE Accounts ------------------------------
(***************************************************************************)
(* The accounts and AIs nexus-server holds (src/accounts/registry.h,   *)
(* src/sync/hub.h, docs/accounts.md): the providers' accounts, the         *)
(* AIs, the links from an AI to the accounts it may run on (in      *)
(* order, with one of them its default, or none), and which account each   *)
(* session runs on.                                                        *)
(*                                                                         *)
(* An account is set up on its own, with its provider, and needs sign-in   *)
(* until a sign-in reports otherwise (`Report`, the server's own step). A  *)
(* AI links to any number of accounts, of any providers, and an        *)
(* account serves any number of AIs. A session is made with an AI   *)
(* (`Create`), on the account of the session it copies when that account   *)
(* is linked to its AI, else on the AI's default. The person sets  *)
(* a session's account to one linked to its AI, or to none             *)
(* (`SetSession`); its harness becomes the account's provider, so a switch *)
(* may cross providers. Multi-subscription AIs default to automatic   *)
(* selection. On a usage limit Rotate moves only automatic sessions, in   *)
(* link order, to another signed-in subscription. CreditHandoff models    *)
(* the exhausted-payer set for the unfinished request.                    *)
(*                                                                         *)
(* The accounts and AIs are also two files a person or an AI edits by  *)
(* hand; `nexus-server config reload` applies an edit (`Reload*`): a       *)
(* AI dropped, a link dropped, a default or an account's provider      *)
(* changed. An edit that leaves the files inconsistent is refused whole    *)
(* and changes nothing.                                                    *)
(*                                                                         *)
(* An AI also has instances: the AI on one machine, in one          *)
(* working directory (named, in order; the first is the AI's default   *)
(* instance), set by edits only (`ReloadAddInstance`,                      *)
(* `ReloadDropInstance`, `ReloadMoveInstance`). Accounts are the           *)
(* AI's, shared by all its instances; a session belongs to one of its  *)
(* AI's instances, or to none when the AI has none. A create takes *)
(* the instance of the session it copies when that is the same AI's    *)
(* and still one of its instances, else the AI's default instance.     *)
(*                                                                         *)
(* An AI dropped by an edit keeps its sessions exactly as they are      *)
(* (account, harness, instance), and each is told; they are settled like   *)
(* the rest once the AI is set up again (`Live`). What the drop does   *)
(* to a session's turn and driver is Supervisor.tla's (`RemoveAI`).    *)
(*                                                                         *)
(* Every change of the accounts and AIs, by an action or an edit,      *)
(* ends in `Settled`: each session on an account no longer linked to its   *)
(* AI moves, in the same step, to the AI's default when that is    *)
(* signed in, else to the AI's first signed-in linked account, else to *)
(* none; a session whose account's provider changed takes the new          *)
(* harness; and each session moved or changed is told (a notice in its     *)
(* transcript). An unlinked or removed default becomes none. A session     *)
(* whose instance its AI no longer has, or on none while its AI    *)
(* has some, moves to the AI's default instance, or none, and is told; *)
(* an instance moved in place (another machine or directory, the same      *)
(* name) tells every session on it.                                        *)
(*                                                                         *)
(* `told` and `last` are history: the sessions given a notice by the step  *)
(* just taken, and the operation with its arguments and result. No action  *)
(* reads either, so TLC tells states apart without them (VIEW).            *)
(*                                                                         *)
(* An AI is limited to its directory or not (`lim`, ais.json's    *)
(* "limit"; an AI set up by an action is limited, one not set up reads *)
(* limited), and an account is billed by subscription or at API rate      *)
(* (`bill`, accounts.json's "billing"); both are set by edits only          *)
(* (`ReloadLimit`, `ReloadBilling`). An AI that is not limited may link  *)
(* no API-billed account: an edit or a link that would leave one is        *)
(* refused. A session's driver starts (`Start`) limited when its AI is  *)
(* limited or its account is billed at API rate, decided then, from the    *)
(* files as they are (accounts::limitedFor); a change later takes effect   *)
(* from the next start (`End`, then `Start`: Supervisor.tla retires the     *)
(* driver once no turn is open), and an AI's limit changed tells each   *)
(* of its sessions. `run` is each session's driver: off, or started limited *)
(* or open, with whether its account was API-billed and its AI limited *)
(* at the start.                                                           *)
(*                                                                         *)
(* Not modelled: an instance's machine and directory (a driver runs only   *)
(* for an instance on the server's own machine: unit- and functional-      *)
(* tested); what a change of a session's account does to its driver        *)
(* (Supervisor.tla), the sessions' transcripts and queues (Link.tla), the  *)
(* files' text and the store's durability, and names' and limits' checks   *)
(* (unit-tested: the accounts_unit tests). AccountsTrace.tla binds the     *)
(* code.                                                                   *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Accts,          \* the account names a model config uses
    Provider,       \* each account's provider when it is set up
    Providers,      \* the providers an edit may give an account
    AIs,        \* the AI names a model config uses
    Insts,          \* the AIs' instance names a model config uses
    MaxSes,         \* the sessions a model config may make (ids 1 .. MaxSes)
    StartHarness,   \* the harness of a session on no account (the AI's harness)
    Bills,          \* the billings an edit may give an account: "sub", "api"
    Limits,         \* the limits an edit may give an AI: TRUE, FALSE
    Runs            \* whether drivers start and end (TRUE) or the model leaves them off (FALSE)

None == "none"
Ids == 1..MaxSes
Range(sq) == {sq[i] : i \in DOMAIN sq}
Harnesses == Providers \cup {StartHarness}
ASSUME None \notin Accts \cup AIs \cup Harnesses \cup Insts
ASSUME \A a \in Accts : Provider[a] \in Providers
ASSUME Bills \subseteq {"sub", "api"} /\ Limits \subseteq BOOLEAN /\ Runs \in BOOLEAN

VARIABLES
    acct,       \* each account: "absent", "in" (signed in) or "out" (needs sign-in)
    prov,       \* each account's provider
    known,      \* the AIs set up
    links,      \* each AI's linked accounts, in link order
    dflt,       \* each AI's default account, or None
    insts,      \* each AI's instances, in order (the first its default instance)
    ses,        \* each session id: made, its AI, its instance (or None), its account (or None) and its harness
    nextSes,    \* the id the next session made gets
    lim,        \* each AI: limited to its directory (TRUE for an AI not set up)
    bill,       \* each account's billing: "sub" or "api" ("sub" for an account not set up)
    run,        \* each session's driver: its state "off", "limited" or "open", and at its start, api and flim
    told,       \* history: the sessions the step gave a notice
    last        \* history: the operation, its arguments and its result

model == <<acct, prov, known, links, dflt, insts, ses, nextSes, lim, bill, run>>
vars == <<acct, prov, known, links, dflt, insts, ses, nextSes, lim, bill, run, told, last>>
View == model

NoSes == [made |-> FALSE, ai |-> None, inst |-> None, acct |-> None, harness |-> None, automatic |-> FALSE]
Off == [st |-> "off", api |-> FALSE, flim |-> FALSE]

Up(a) == acct[a] = "in"
Drop(sq, x) == SelectSeq(sq, LAMBDA y : y /= x)
Pos(sq, x) == IF x \in Range(sq) THEN CHOOSE i \in DOMAIN sq : sq[i] = x ELSE 0
\* The accounts after x in sq, in order, cycling round; x itself never (all of sq when x is not in it).
After(sq, x) == LET i == Pos(sq, x) IN
    IF i = 0 THEN sq ELSE SubSeq(sq, i + 1, Len(sq)) \o SubSeq(sq, 1, i - 1)
FirstUp(sq) == LET c == SelectSeq(sq, Up) IN IF c = << >> THEN None ELSE Head(c)
\* The AI's next signed-in linked account after x, in link order, or None.
Subs(sq, nb) == SelectSeq(sq, LAMBDA a : nb[a] = "sub")
NextUp(f, x) == FirstUp(After(Subs(links[f], bill), x))
HarnessFor(a, p, h) == IF a = None THEN h ELSE p[a]
FirstOr(sq) == IF sq = << >> THEN None ELSE Head(sq)

\* After links nl, defaults nd and providers np take effect: a session on an account no longer linked to its AI
\* (stray) moves to the AI's default when signed in, else its first signed-in linked account, else none; a session
\* on an account kept takes its provider's harness. Told: every session moved or whose harness changed.
Stray(nl, s) == ses[s].made /\ ses[s].acct /= None /\ ses[s].acct \notin Range(nl[ses[s].ai])
Landing(nl, nd, f) == IF nd[f] /= None /\ Up(nd[f]) THEN nd[f] ELSE FirstUp(nl[f])
\* A session whose instance its AI no longer has (after instances ni), or on none while its AI has some,
\* moves to the AI's first, or none.
StrayInst(ni, s) == ses[s].made /\ IF ses[s].inst = None THEN ni[ses[s].ai] /= << >>
                                   ELSE ses[s].inst \notin Range(ni[ses[s].ai])
InstOf(ni, s) == IF StrayInst(ni, s) THEN FirstOr(ni[ses[s].ai]) ELSE ses[s].inst
\* A session whose AI is not set up (after AIs kn) is left as it is, its account, harness and instance kept, and
\* settled like the rest once its AI is set up again. One whose AI the step took out is told (its work is
\* stopped: Supervisor.tla RemoveAI).
Out(kn, s) == ses[s].ai \notin kn
FirstAuto(subs) == IF FirstUp(subs) /= None THEN FirstUp(subs) ELSE FirstOr(subs)
SettledAccount(nl, nd, nb, s) ==
    LET f == ses[s].ai
        subs == Subs(nl[f], nb)
    IN IF ses[s].automatic THEN IF ses[s].acct \in Range(subs) THEN ses[s].acct ELSE FirstAuto(subs)
       ELSE IF Stray(nl, s) THEN Landing(nl, nd, f) ELSE ses[s].acct

SettledWithBilling(nl, nd, np, ni, kn, nb) ==
    [s \in Ids |-> IF Out(kn, s) THEN ses[s] ELSE
        LET a == SettledAccount(nl, nd, nb, s)
        IN [ses[s] EXCEPT !.acct = a, !.automatic = @ /\ Len(Subs(nl[ses[s].ai], nb)) > 1,
                          !.harness = HarnessFor(a, np, @), !.inst = InstOf(ni, s)]]
SettledI(nl, nd, np, ni, kn) == SettledWithBilling(nl, nd, np, ni, kn, bill)
SettledToldI(nl, np, ni, kn) ==
    {s \in Ids : ~Out(kn, s) /\ (Stray(nl, s) \/ StrayInst(ni, s)
                                 \/ (ses[s].automatic /\ Len(Subs(nl[ses[s].ai], bill)) < 2)
                                 \/ (ses[s].made /\ ses[s].acct /= None /\ np[ses[s].acct] /= ses[s].harness))}
    \cup {s \in Ids : ses[s].made /\ ses[s].ai \in known \ kn}
\* An AI not set up has no instances.
InstsFor(kn) == [f \in AIs |-> IF f \in kn THEN insts[f] ELSE << >>]
Settled(nl, nd, np) == SettledI(nl, nd, np, insts, known)
SettledTold(nl, np) == SettledToldI(nl, np, insts, known)

Done(op, f, a, s, ok) == last' = [op |-> op, f |-> f, a |-> a, s |-> s, ok |-> ok]
Refused(op, f, a, s, notice) ==
    /\ Done(op, f, a, s, FALSE)
    /\ told' = notice
    /\ UNCHANGED model

\* The accounts and AIs become nl, nd, np, kn, ac, ni, and the sessions are settled, in one step. mv: the
\* instances moved in place (<<AI, instance>>: another machine or directory), whose sessions are told too.
BecomeIM(op, f, a, ac, np, kn, nl, nd, ni, mv) ==
    /\ acct' = ac /\ prov' = np /\ known' = kn /\ links' = nl /\ dflt' = nd /\ insts' = ni
    /\ ses' = SettledI(nl, nd, np, ni, kn)
    /\ told' = SettledToldI(nl, np, ni, kn) \cup {s \in Ids : ses[s].made /\ <<ses[s].ai, ses[s].inst>> \in mv}
    /\ Done(op, f, a, 0, TRUE)
    \* An AI kept keeps its limit; one set up anew, or not set up, is limited. An account not set up bills "sub".
    /\ lim' = [g \in AIs |-> IF g \in kn /\ g \in known THEN lim[g] ELSE TRUE]
    /\ bill' = [b \in Accts |-> IF ac[b] = "absent" THEN "sub" ELSE bill[b]]
    /\ UNCHANGED <<nextSes, run>>
BecomeI(op, f, a, ac, np, kn, nl, nd, ni) == BecomeIM(op, f, a, ac, np, kn, nl, nd, ni, {})
Become(op, f, a, ac, np, kn, nl, nd) == BecomeI(op, f, a, ac, np, kn, nl, nd, InstsFor(kn))

\* The files agree: links name accounts set up, each once, only from AIs set up; a default is linked or none;
\* instances only of AIs set up, each named once; an AI not limited (limits nm) links no account billed at API
\* rate (billings nb).
NoApiUnlimited(kn, nl, nm, nb) == \A f \in kn : ~nm[f] => \A i \in DOMAIN nl[f] : nb[nl[f][i]] /= "api"
ConsistentLB(ac, kn, nl, nd, ni, nm, nb) ==
    /\ \A f \in AIs :
        /\ \A i \in DOMAIN nl[f] : ac[nl[f][i]] /= "absent"
        /\ Len(nl[f]) = Cardinality(Range(nl[f]))
        /\ f \notin kn => nl[f] = << >> /\ nd[f] = None /\ ni[f] = << >>
        /\ nd[f] = None \/ nd[f] \in Range(nl[f])
        /\ Len(ni[f]) = Cardinality(Range(ni[f]))
    /\ NoApiUnlimited(kn, nl, nm, nb)
ConsistentI(ac, kn, nl, nd, ni) == ConsistentLB(ac, kn, nl, nd, ni, lim, bill)
Consistent(ac, kn, nl, nd) == ConsistentI(ac, kn, nl, nd, InstsFor(kn))

Init ==
    /\ acct = [a \in Accts |-> "absent"]
    /\ prov = Provider
    /\ known = {}
    /\ links = [f \in AIs |-> << >>]
    /\ dflt = [f \in AIs |-> None]
    /\ insts = [f \in AIs |-> << >>]
    /\ ses = [s \in Ids |-> NoSes]
    /\ nextSes = 1
    /\ lim = [f \in AIs |-> TRUE]
    /\ bill = [a \in Accts |-> "sub"]
    /\ run = [s \in Ids |-> Off]
    /\ told = {}
    /\ last = [op |-> "init", f |-> None, a |-> None, s |-> 0, ok |-> TRUE]

\* The person's: an account set up, with its provider; it needs sign-in until a sign-in reports otherwise.
AddAccount(a) ==
    IF acct[a] = "absent"
    THEN Become("add", None, a, [acct EXCEPT ![a] = "out"], prov, known, links, dflt)
    ELSE Refused("add", None, a, 0, {})

\* The server's: a sign-in (or its loss) reported for an account.
Report(a, st) ==
    IF acct[a] /= "absent"
    THEN /\ acct' = [acct EXCEPT ![a] = st]
         /\ Done(IF st = "in" THEN "in" ELSE "out", None, a, 0, TRUE) /\ told' = {}
         /\ UNCHANGED <<prov, known, links, dflt, insts, ses, nextSes, lim, bill, run>>
    ELSE Refused(IF st = "in" THEN "in" ELSE "out", None, a, 0, {})

\* The person's: an account removed. Every AI's link to it goes, and a default on it becomes none.
RemoveAccount(a) ==
    IF acct[a] /= "absent"
    THEN Become("remove", None, a, [acct EXCEPT ![a] = "absent"], prov, known,
                [f \in AIs |-> Drop(links[f], a)], [f \in AIs |-> IF dflt[f] = a THEN None ELSE dflt[f]])
    ELSE Refused("remove", None, a, 0, {})

AddAI(f) ==
    IF f \notin known
    THEN Become("ai", f, None, acct, prov, known \cup {f}, links, dflt)
    ELSE Refused("ai", f, None, 0, {})

\* Refused, too, when the AI is not limited and the account is billed at API rate.
LinkAccount(f, a) ==
    IF f \in known /\ acct[a] /= "absent" /\ a \notin Range(links[f]) /\ (lim[f] \/ bill[a] /= "api")
    THEN Become("link", f, a, acct, prov, known, [links EXCEPT ![f] = Append(@, a)], dflt)
    ELSE Refused("link", f, a, 0, {})

\* The person's: the AI no longer runs on the account; a default on it becomes none.
Unlink(f, a) ==
    IF a \in Range(links[f])
    THEN Become("unlink", f, a, acct, prov, known, [links EXCEPT ![f] = Drop(@, a)],
                [dflt EXCEPT ![f] = IF @ = a THEN None ELSE @])
    ELSE Refused("unlink", f, a, 0, {})

SetDefault(f, a) ==
    IF f \in known /\ (a = None \/ a \in Range(links[f]))
    THEN Become("default", f, a, acct, prov, known, links, [dflt EXCEPT ![f] = a])
    ELSE Refused("default", f, a, 0, {})

\* Hand edits of the files, applied by a reload: each the whole of what the files say, refused when inconsistent.
ReloadI(f, a, ac, np, kn, nl, nd, ni) ==
    IF ConsistentI(ac, kn, nl, nd, ni)
    THEN BecomeI("reload", f, a, ac, np, kn, nl, nd, ni)
    ELSE Refused("reload", f, a, 0, {})
Reload(f, a, ac, np, kn, nl, nd) == ReloadI(f, a, ac, np, kn, nl, nd, InstsFor(kn))

\* An AI taken out of the AIs file, its links and default with it.
ReloadDropAI(f) ==
    Reload(f, None, acct, prov, known \ {f}, [links EXCEPT ![f] = << >>], [dflt EXCEPT ![f] = None])
\* A link taken out of an AI's list, its default left as it was (refused when that was the link).
ReloadDropLink(f, a) == Reload(f, a, acct, prov, known, [links EXCEPT ![f] = Drop(@, a)], dflt)
\* An account taken out of the accounts file, the links left as they were (refused while linked).
ReloadDropAccount(a) == Reload(None, a, [acct EXCEPT ![a] = "absent"], prov, known, links, dflt)
\* An account's provider changed.
ReloadProvider(a, p) ==
    LET ac == IF prov[a] /= p /\ acct[a] = "in" THEN [acct EXCEPT ![a] = "out"] ELSE acct
    IN Reload(None, a, ac, [prov EXCEPT ![a] = p], known, links, dflt)
\* An AI's limit set (refused for an AI not set up, or false while it links an account billed at API rate).
\* When it changes, each of the AI's sessions is told (tightened, an open turn stops and the driver restarts limited;
\* loosened, once no turn is open: Supervisor.tla SetLimit).
ReloadLimit(f, v) ==
    IF f \in known /\ ConsistentLB(acct, known, links, dflt, insts, [lim EXCEPT ![f] = v], bill)
    THEN /\ lim' = [lim EXCEPT ![f] = v]
         /\ told' = IF v = lim[f] THEN {} ELSE {s \in Ids : ses[s].made /\ ses[s].ai = f}
         /\ Done("limit", f, None, 0, TRUE)
         /\ UNCHANGED <<acct, prov, known, links, dflt, insts, ses, nextSes, bill, run>>
    ELSE Refused("limit", f, None, 0, {})
\* An account's billing set (refused for an account not set up, or "api" while an AI not limited links it).
ReloadBilling(a, b) ==
    IF acct[a] /= "absent" /\ ConsistentLB(acct, known, links, dflt, insts, lim, [bill EXCEPT ![a] = b])
    THEN /\ bill' = [bill EXCEPT ![a] = b]
         /\ ses' = SettledWithBilling(links, dflt, prov, insts, known, bill')
         /\ told' = {s \in Ids : ses'[s] /= ses[s]}
         /\ Done("billing", None, a, 0, TRUE)
         /\ UNCHANGED <<acct, prov, known, links, dflt, insts, nextSes, lim, run>>
    ELSE Refused("billing", None, a, 0, {})
\* An instance added to the end of an AI's (refused for an AI not set up, or a name it already has).
ReloadAddInstance(f, i) == ReloadI(f, i, acct, prov, known, links, dflt, [insts EXCEPT ![f] = Append(@, i)])
\* An instance taken out of an AI's.
ReloadDropInstance(f, i) == ReloadI(f, i, acct, prov, known, links, dflt, [insts EXCEPT ![f] = Drop(@, i)])
\* An instance of an AI's moved in place: its machine or directory edited, its name kept (the names alone are
\* modelled, so nothing else changes; refused for an instance the AI does not have).
ReloadMoveInstance(f, i) ==
    IF i \in Range(insts[f])
    THEN BecomeIM("moveinst", f, i, acct, prov, known, links, dflt, insts, {<<f, i>>})
    ELSE Refused("moveinst", f, i, 0, {})

\* The person's: the session's account, one linked to its AI or none, whatever its provider or sign-in. A
\* refusal is a notice in the session's transcript.
SetSession(s, a) ==
    IF ses[s].made /\ (a = None \/ a \in Range(links[ses[s].ai]))
    THEN /\ ses' = [ses EXCEPT ![s].acct = a, ![s].automatic = FALSE, ![s].harness = HarnessFor(a, prov, @)]
         /\ Done("set", None, a, s, TRUE)
         /\ told' = IF a /= None /\ ses[s].acct /= None /\ ses[s].acct /= a THEN {s} ELSE {}
         /\ UNCHANGED <<acct, prov, known, links, dflt, insts, nextSes, lim, bill, run>>
    ELSE Refused("set", None, a, s, IF ses[s].made THEN {s} ELSE {})

\* The server's, where a usage limit is reported: the AI's next signed-in linked account after the session's, in
\* link order. The session is told either way: of the switch, or that there is none to switch to.
RotateTo(s, n) ==
    IF ses[s].made
    THEN IF n = None
         THEN Refused("rotate", None, None, s, {s})
         ELSE /\ ses' = [ses EXCEPT ![s].acct = n, ![s].harness = prov[n]]
              /\ Done("rotate", None, n, s, TRUE) /\ told' = {s}
              /\ UNCHANGED <<acct, prov, known, links, dflt, insts, nextSes, lim, bill, run>>
    ELSE Refused("rotate", None, None, s, {})

Rotate(s) == IF ses[s].automatic THEN RotateTo(s, NextUp(ses[s].ai, ses[s].acct))
             ELSE Refused("rotate", None, None, s, {})

\* A fork may decline the move or pick any other signed-in linked account.
PolicyRotate(s) ==
    IF ses[s].automatic
    THEN \E n \in {None} \cup {a \in Range(Subs(links[ses[s].ai], bill)) : a /= ses[s].acct /\ Up(a)} : RotateTo(s, n)
    ELSE Refused("rotate", None, None, s, {})

\* A session made with AI f, copying session `from` (0: none): its account when linked to f (or none), else
\* f's default; its instance when `from` is f's and f still has it, else f's default instance. Past the limit, or
\* copying a session not made, refused.
Create(f, from) ==
    IF nextSes <= MaxSes /\ (from = 0 \/ ses[from].made)
    THEN LET subs == Subs(links[f], bill)
             same == from /= 0 /\ ses[from].ai = f
             auto == Len(subs) > 1 /\ (~same \/ ses[from].automatic)
             a == IF auto THEN IF same /\ ses[from].acct \in Range(subs) /\ Up(ses[from].acct)
                               THEN ses[from].acct ELSE FirstAuto(subs)
                  ELSE IF from /= 0 /\ (ses[from].acct = None \/ ses[from].acct \in Range(links[f]))
                  THEN ses[from].acct ELSE dflt[f]
             i == IF from /= 0 /\ ses[from].ai = f /\ ses[from].inst \in Range(insts[f])
                  THEN ses[from].inst ELSE FirstOr(insts[f]) IN
         /\ ses' = [ses EXCEPT ![nextSes] = [made |-> TRUE, ai |-> f, inst |-> i, acct |-> a,
                                             harness |-> HarnessFor(a, prov, StartHarness), automatic |-> auto]]
         /\ nextSes' = nextSes + 1
         /\ Done("create", f, a, from, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, prov, known, links, dflt, insts, lim, bill, run>>
    ELSE Refused("create", f, None, from, {})

\* The server's: a session's driver started, limited when its AI is limited or its account is billed at API rate,
\* from the files as they are now (accounts::limitedFor). Eff is passed in so that a witness can break it. An AI or
\* an account the files no longer hold reads limited: the safe direction.
Eff(s) == \/ ses[s].ai \notin known
          \/ lim[ses[s].ai]
          \/ ses[s].acct /= None /\ (acct[ses[s].acct] = "absent" \/ bill[ses[s].acct] = "api")
StartWith(E(_), s) ==
    IF ses[s].made /\ run[s].st = "off"
    THEN /\ run' = [run EXCEPT ![s] = [st |-> IF E(s) THEN "limited" ELSE "open",
                                       api |-> ses[s].acct /= None /\ bill[ses[s].acct] = "api",
                                       flim |-> lim[ses[s].ai]]]
         /\ Done("start", None, None, s, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, prov, known, links, dflt, insts, ses, nextSes, lim, bill>>
    ELSE Refused("start", None, None, s, {})
Start(s) == StartWith(Eff, s)
\* The server's: a session's driver ended (retired after a change, exited, stopped).
End(s) ==
    IF run[s].st /= "off"
    THEN /\ run' = [run EXCEPT ![s] = Off]
         /\ Done("end", None, None, s, TRUE) /\ told' = {}
         /\ UNCHANGED <<acct, prov, known, links, dflt, insts, ses, nextSes, lim, bill>>
    ELSE Refused("end", None, None, s, {})

\* The steps, with the actions a reversed witness breaks passed in.
NextAll(Add(_), Rm(_), Ul(_, _), Def(_, _), St(_, _), Rot(_), Cr(_, _), DropLink(_, _), Pv(_, _), AddInst(_, _),
        DropInst(_, _), MoveInst(_, _), Lk(_, _), AddF(_), Sta(_), LimR(_, _), BillR(_, _)) ==
    \/ \E f \in AIs, i \in Insts : AddInst(f, i) \/ DropInst(f, i) \/ MoveInst(f, i)
    \/ \E a \in Accts : Add(a) \/ Rm(a) \/ Report(a, "in") \/ Report(a, "out") \/ ReloadDropAccount(a)
    \/ \E a \in Accts, p \in Providers : Pv(a, p)
    \/ \E f \in AIs : AddF(f) \/ ReloadDropAI(f)
    \/ \E f \in AIs, a \in Accts : Lk(f, a) \/ Ul(f, a) \/ DropLink(f, a)
    \/ \E f \in AIs, v \in Limits : LimR(f, v)
    \/ \E a \in Accts, b \in Bills : BillR(a, b)
    \/ Runs /\ \E s \in Ids : Sta(s) \/ End(s)
    \/ \E f \in AIs, a \in Accts \cup {None} : Def(f, a)
    \/ \E s \in Ids, a \in Accts \cup {None} : St(s, a)
    \/ \E s \in Ids : Rot(s)
    \/ \E f \in AIs, from \in 0..MaxSes : Cr(f, from)

NextWithIM(Add(_), Rm(_), Ul(_, _), Def(_, _), St(_, _), Rot(_), Cr(_, _), DropLink(_, _), Pv(_, _), AddInst(_, _),
           DropInst(_, _), MoveInst(_, _)) ==
    NextAll(Add, Rm, Ul, Def, St, Rot, Cr, DropLink, Pv, AddInst, DropInst, MoveInst, LinkAccount, AddAI, Start,
            ReloadLimit, ReloadBilling)

\* The steps with the limit's actions passed in, for its witnesses.
NextLimitWith(Lk(_, _), AddF(_), Sta(_), LimR(_, _), BillR(_, _)) ==
    NextAll(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, Rotate, Create, ReloadDropLink, ReloadProvider,
            ReloadAddInstance, ReloadDropInstance, ReloadMoveInstance, Lk, AddF, Sta, LimR, BillR)

NextWithI(Add(_), Rm(_), Ul(_, _), Def(_, _), St(_, _), Rot(_), Cr(_, _), DropLink(_, _), Pv(_, _), DropInst(_, _)) ==
    NextWithIM(Add, Rm, Ul, Def, St, Rot, Cr, DropLink, Pv, ReloadAddInstance, DropInst, ReloadMoveInstance)

NextWith(Add(_), Rm(_), Ul(_, _), Def(_, _), St(_, _), Rot(_), Cr(_, _), DropLink(_, _), Pv(_, _)) ==
    NextWithI(Add, Rm, Ul, Def, St, Rot, Cr, DropLink, Pv, ReloadDropInstance)

Next == NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, Rotate, Create, ReloadDropLink,
                    ReloadProvider)

Spec == Init /\ [][Next]_vars

PolicyNext == NextWith(AddAccount, RemoveAccount, Unlink, SetDefault, SetSession, PolicyRotate, Create, ReloadDropLink,
                       ReloadProvider)
PolicySpec == Init /\ [][PolicyNext]_vars

-----------------------------------------------------------------------------
\* Invariants.

Session == [made : BOOLEAN, ai : AIs \cup {None}, inst : Insts \cup {None}, acct : Accts \cup {None},
            harness : Harnesses \cup {None}, automatic : BOOLEAN]

TypeOK ==
    /\ acct \in [Accts -> {"absent", "in", "out"}]
    /\ prov \in [Accts -> Providers]
    /\ known \subseteq AIs
    /\ \A f \in AIs : links[f] \in Seq(Accts)
    /\ dflt \in [AIs -> Accts \cup {None}]
    /\ \A f \in AIs : insts[f] \in Seq(Insts)
    /\ ses \in [Ids -> Session]
    /\ nextSes \in 1..(MaxSes + 1)
    /\ lim \in [AIs -> BOOLEAN]
    /\ bill \in [Accts -> {"sub", "api"}]
    /\ run \in [Ids -> [st : {"off", "limited", "open"}, api : BOOLEAN, flim : BOOLEAN]]
    /\ told \subseteq Ids
    /\ last.ok \in BOOLEAN

\* Links name accounts set up, each once, only from AIs set up.
LinksSound ==
    \A f \in AIs :
        /\ \A i \in DOMAIN links[f] : acct[links[f][i]] /= "absent"
        /\ Len(links[f]) = Cardinality(Range(links[f]))
        /\ f \notin known => links[f] = << >>

\* An AI's default is one of its linked accounts, or none.
DefaultLinked == \A f \in AIs : dflt[f] = None \/ dflt[f] \in Range(links[f])

\* A session of an AI set up: one made whose AI is in the AIs file. A session whose AI was taken out is
\* kept as it was, and held to these again once its AI is set up again (the step that sets it up settles it).
Live(s) == ses[s].made /\ ses[s].ai \in known

\* A session runs on an account linked to its AI, or on none: never on one unlinked or removed.
SessionLinked ==
    \A s \in Ids : Live(s) => (ses[s].acct = None \/ ses[s].acct \in Range(links[ses[s].ai]))

\* A session belongs to one of its AI's instances, or to none when its AI has none; instances only of AIs
\* set up, each named once.
InstanceHeld ==
    /\ \A s \in Ids : Live(s) => IF insts[ses[s].ai] = << >> THEN ses[s].inst = None
                                 ELSE ses[s].inst \in Range(insts[ses[s].ai])
    /\ \A f \in AIs : Len(insts[f]) = Cardinality(Range(insts[f])) /\ (f \notin known => insts[f] = << >>)

\* A session on an account runs its provider's harness.
HarnessMatches == \A s \in Ids : Live(s) /\ ses[s].acct /= None => ses[s].harness = prov[ses[s].acct]

\* The sessions made are exactly the ids below the next.
IdsDense == \A s \in Ids : ses[s].made <=> s < nextSes

\* No driver of a session on an account billed at API rate ever starts unlimited.
ApiAlwaysLimited == \A s \in Ids : run[s].st /= "off" /\ run[s].api => run[s].st = "limited"

\* No driver starts with a weaker limit than its AI's at its start.
NeverWeakerThanAI == \A s \in Ids : run[s].st /= "off" /\ run[s].flim => run[s].st = "limited"

\* An AI not limited links no account billed at API rate; an AI not set up reads limited, an account not set up
\* bills by subscription; only a session made has a driver.
LimitSound ==
    /\ NoApiUnlimited(known, links, lim, bill)
    /\ \A f \in AIs \ known : lim[f]
    /\ \A a \in Accts : acct[a] = "absent" => bill[a] = "sub"
    /\ \A s \in Ids : run[s].st /= "off" => ses[s].made

-----------------------------------------------------------------------------
\* Action properties, on the step's history.

\* A session's account, harness or instance changes without a notice to it only when the person set its account.
MovedIsTold ==
    [][\A s \in Ids : ses[s].made /\ (ses'[s].acct /= ses[s].acct \/ ses'[s].harness /= ses[s].harness
                                      \/ ses'[s].inst /= ses[s].inst)
                      /\ ~(last'.op = "set" /\ last'.s = s)
                        => s \in told']_vars

\* A rotation always lands on a signed-in subscription. On reload auto may land on a signed-out
\* subscription only when every linked subscription is signed out (to offer its sign-in).
ServerPicksSignedIn ==
    [][\A s \in Ids : ses[s].made /\ ses'[s].acct /= ses[s].acct /\ ses'[s].acct /= None /\ last'.op /= "set"
                      /\ (~ses[s].automatic \/ last'.op = "rotate"
                          \/ (\E a \in Range(Subs(links'[ses[s].ai], bill')) : acct'[a] = "in"))
                        => acct'[ses'[s].acct] = "in"]_vars

\* Automatic selection moves when the AI has another signed-in subscription.
RotateMoves ==
    [][last'.op = "rotate" /\ ses[last'.s].automatic
       /\ (\E b \in Range(Subs(links[ses[last'.s].ai], bill)) : b /= ses[last'.s].acct /\ acct[b] = "in")
         => ses'[last'.s].acct /= ses[last'.s].acct]_vars

\* A set is taken exactly for a session made and an account linked to its AI (or none), and gives it that one.
SetExact ==
    [][last'.op = "set" =>
         /\ last'.ok <=> (ses[last'.s].made /\ (last'.a = None \/ last'.a \in Range(links[ses[last'.s].ai])))
         /\ last'.ok => ses'[last'.s].acct = last'.a]_vars

\* A set or a rotation changes no other session.
OnlyItsSession == [][last'.op \in {"set", "rotate"} => \A s \in Ids \ {last'.s} : ses'[s] = ses[s]]_vars

\* An instance moved in place tells every session on it (its driver follows it there).
MovedInstanceIsTold ==
    [][last'.op = "moveinst" /\ last'.ok =>
         {s \in Ids : ses[s].made /\ ses[s].ai = last'.f /\ ses[s].inst = last'.a} \subseteq told']_vars

\* An AI set up by an action is limited.
NewAILimited == [][last'.op = "ai" /\ last'.ok => lim'[last'.f]]_vars

\* An AI's limit changed tells each of its sessions.
LimitChangeIsTold ==
    [][\A s \in Ids : ses[s].made /\ known' = known /\ lim'[ses[s].ai] /= lim[ses[s].ai] => s \in told']_vars

\* An operation refused changes nothing.
RefusedChangesNothing == [][~last'.ok => UNCHANGED model]_vars

\* A session whose AI is out of the AIs file after the step is left exactly as it was by every change of the
\* accounts and AIs (the person's own set aside); one whose AI the step took out is told.
OutLeftAlone ==
    [][last'.op /= "set" =>
         \A s \in Ids : ses[s].made /\ ses[s].ai \notin known' => ses'[s] = ses[s]]_vars
OutIsTold == [][\A s \in Ids : ses[s].made /\ ses[s].ai \in known \ known' => s \in told']_vars

=============================================================================

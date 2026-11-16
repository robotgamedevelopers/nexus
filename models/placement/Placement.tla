----------------------------- MODULE Placement -----------------------------
(***************************************************************************)
(* Placement: installed builds are immutable, complete, and switched       *)
(* atomically (src/server/builds.h, docs/builds.md).                       *)
(*                                                                         *)
(* The builds directory holds build folders, each named by the content     *)
(* hash of what is in it (B(t): tree t's name), pair folders (P(c, p): two *)
(* links, current to c and previous to p), and one pointer, the link       *)
(* `active`, naming the pair in force. Placing writes a build into a       *)
(* temporary folder (T), syncs it, verifies it, and renames it to its      *)
(* hash name; switching makes a pair folder (Q, then its P name) and       *)
(* renames a new pointer link (N) over `active`. Every mutating verb runs  *)
(* under the builds directory's lock, syncs the directory, and removes the *)
(* leftovers of interrupted runs (T, Q, N, G names, and pairs `active`     *)
(* does not name). Pruning takes a build folder's exclusive lock, renames  *)
(* it aside (G) and removes it; a running server holds its own build       *)
(* folder's lock shared.                                                   *)
(*                                                                         *)
(* Each action is one step of the code (src/server/builds.cpp; its phase   *)
(* names are the pc values here), for one actor (a process running a      *)
(* verb). Any two actors may run at once; between any two steps an actor   *)
(* may die (kill -9: the disk as it is, its locks released by the OS) or   *)
(* the machine may crash: each folder's contents as last synced or as      *)
(* now, and of the directory's changes since its last sync any subset,     *)
(* applied in order (a rename is one change: both names or neither).       *)
(*                                                                         *)
(* A tree is a number; its program and driver tree are Prog(t) and Drv(t): *)
(* tree 1 is program 1 with driver 1, tree 2 program 2 with driver 2, and  *)
(* tree 3 program 1 with driver 2 (tree 1 with its driver changed after    *)
(* its tests). A build's parts are written in the code's order, 1 bin/,    *)
(* 2 drivers/, 3 formats; each part records the tree whose bytes it holds. *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Actors,       \* the processes running verbs (place, current, back, prune)
    Servers,      \* nexus-server processes started from a build
    Trees,        \* the trees that may be placed (a subset of 1..3)
    Tested,       \* the trees whose tests passed: an evidence file names each
    Initial,      \* the build placed and current at the start (in Tested)
    Others,       \* the builds placed beside it at the start (tested or not, as Tested says)
    MaxOps,       \* verbs each actor runs
    MaxCrashes,   \* machine crashes
    MaxDeaths,    \* actors killed
    MaxServes,    \* server starts
    MaxFolders,   \* folder ids: 4, and one for every placement and pair the instance makes
    Bug           \* "none" (the code), or one reversed witness's break

ASSUME Trees \subseteq 1..3 /\ Tested \subseteq Trees /\ Initial \in Tested /\ Others \subseteq Trees \ {Initial}
ASSUME MaxFolders >= 4

Placed0 == {Initial} \cup Others
Prog(t) == IF t = 2 THEN 2 ELSE 1
Drv(t) == IF t = 1 THEN 1 ELSE 2

None == "none"
Parts == 1..3
Fids == 1..MaxFolders

\* Names in the builds directory, all one record type: k is the kind.
B(t) == [k |-> "b", x |-> t, y |-> 0]      \* a build, named by its hash
P(c, p) == [k |-> "p", x |-> c, y |-> p]   \* a pair: current c, previous p (0: none)
T(f) == [k |-> "t", x |-> f, y |-> 0]      \* .tmp-*: a build being placed
Q(f) == [k |-> "q", x |-> f, y |-> 0]      \* .pair-*: a pair being made
N(f) == [k |-> "n", x |-> f, y |-> 0]      \* .active-*: a new pointer link, not yet renamed
G(f) == [k |-> "g", x |-> f, y |-> 0]      \* .gone-*: a build being pruned
NoName == [k |-> "-", x |-> 0, y |-> 0]

Free == [kind |-> "free", parts |-> [i \in Parts |-> 0], man |-> 0, mark |-> FALSE, c |-> 0, p |-> 0]
\* A folder's content as sealed: what it must keep while it is named.
Content(r) == [parts |-> r.parts, man |-> r.man, mark |-> r.mark]
NoContent == [parts |-> [i \in Parts |-> 0], man |-> 0, mark |-> FALSE]
NoAct == [pn |-> NoName, g |-> 0]

Verbs == {"place", "switch", "back", "prune"}

VARIABLES
    pc,       \* each actor's next step
    verb,     \* the verb it runs
    tgt,      \* place: the tree; switch: the target build; prune: the build being removed
    ev,       \* place: the tree its evidence names (0: none given)
    wf,       \* the folder it works on
    np,       \* place: the next part to write
    frm,      \* switch: the current build it read at its check
    ph,       \* switch: 1, the first of two pointer renames (two_renames only); 2, the one rename
    cands,    \* prune: the builds still to consider
    lock,     \* the builds directory's lock: its holder, or None
    pex,      \* prune's exclusive lock on a build folder, per actor (0: none)
    fl,       \* each folder as it is now
    fd,       \* each folder as it would survive a crash
    dir,      \* the builds directory's names now: name -> folder
    dirD,     \* its names as last synced
    act,      \* the pointer now: the pair it names, and g, the build that was current before the current one
    actD,     \* the pointer as last synced
    pend,     \* the directory's changes since its last sync, in order (the pointer's rename among them)
    sealed,   \* ghost: each folder's content when it was renamed to its hash name
    srv,      \* each server: down, resolved (it found its build's path) or up (holding its folder shared)
    nextF, ops, crashes, deaths, serves

vars == <<pc, verb, tgt, ev, wf, np, frm, ph, cands, lock, pex, fl, fd, dir, dirD, act, actD, pend, sealed, srv,
          nextF, ops, crashes, deaths, serves>>

Cur == act.pn.x
Prev == act.pn.y
Remove(d, n) == [m \in DOMAIN d \ {n} |-> d[m]]
Add(d, n, f) == (n :> f) @@ d
Held == {srv[s].f : s \in {s \in Servers : srv[s].st = "up"}}
Whole(f, t) == fl[f].kind = "build" /\ \A i \in Parts : fl[f].parts[i] = t /\ fl[f].man = t
BuildOK(t) == B(t) \in DOMAIN dir /\ Whole(dir[B(t)], t)
Leftover(n) == \/ n.k \in {"q", "n", "g"}
               \/ n.k = "t" /\ Bug # "reuse_tmp"
               \/ n.k = "p" /\ n # act.pn
HasLeftover == \E n \in DOMAIN dir : Leftover(n)
TmpOf(f) == {n \in DOMAIN dir : n.k = "t" /\ dir[n] = f}
After(a) == CASE verb[a] = "place" -> "mktmp"
              [] verb[a] = "prune" -> "plist"
              [] OTHER -> "check"
\* The pair a switch makes, by name: (target, the current it read); two_renames' first rename (that current, twice).
PairOf(a) == P(IF ph[a] = 1 THEN frm[a] ELSE tgt[a], frm[a])
SwitchPcs == {"check", "pmk", "psync", "prename", "pdsync", "anew", "arename", "async"}

\* One change to the directory: the name `from` removed and the name `to` made for folder f (a rename is both at once),
\* or the pointer set to `a`.
Op(from, to, f, a) == [from |-> from, to |-> to, f |-> f, a |-> a]
ApplyD(d, o) == LET r == IF o.from # NoName THEN Remove(d, o.from) ELSE d
                IN IF o.to # NoName THEN Add(r, o.to, o.f) ELSE r
ApplyA(p, o) == IF o.a # NoAct THEN o.a ELSE p
RECURSIVE Replay(_, _, _)
Replay(d, p, s) == IF s = <<>> THEN <<d, p>> ELSE Replay(ApplyD(d, Head(s)), ApplyA(p, Head(s)), Tail(s))
RECURSIVE Pick(_, _, _)
Pick(s, keep, i) == IF i > Len(s) THEN <<>> ELSE (IF i \in keep THEN <<s[i]>> ELSE <<>>) \o Pick(s, keep, i + 1)
\* The live directory after a change, and the change recorded.
Change(o) == /\ dir' = ApplyD(dir, o)
             /\ act' = ApplyA(act, o)
             /\ pend' = Append(pend, o)

Init ==
    /\ pc = [a \in Actors |-> "idle"]
    /\ verb = [a \in Actors |-> "place"]
    /\ tgt = [a \in Actors |-> 0]
    /\ ev = [a \in Actors |-> 0]
    /\ wf = [a \in Actors |-> 0]
    /\ np = [a \in Actors |-> 1]
    /\ frm = [a \in Actors |-> 0]
    /\ ph = [a \in Actors |-> 2]
    /\ cands = [a \in Actors |-> {}]
    /\ lock = None
    /\ pex = [a \in Actors |-> 0]
    \* Build t's folder is t (1 to 3), the first pair's is 4, and folders from 5 on are made by the actors.
    /\ fl = [f \in Fids |-> CASE f \in Placed0 -> [Free EXCEPT !.kind = "build", !.parts = [i \in Parts |-> f],
                                                                !.man = f, !.mark = (f \in Tested)]
                              [] f = 4 -> [Free EXCEPT !.kind = "pair", !.c = Initial]
                              [] OTHER -> Free]
    /\ fd = fl
    /\ dir = [n \in {B(t) : t \in Placed0} \cup {P(Initial, 0)} |-> IF n.k = "b" THEN n.x ELSE 4]
    /\ dirD = dir
    /\ act = [pn |-> P(Initial, 0), g |-> 0]
    /\ actD = act
    /\ pend = <<>>
    /\ sealed = [f \in Fids |-> IF f \in Placed0 THEN Content(fl[f]) ELSE NoContent]
    /\ srv = [s \in Servers |-> [st |-> "down", n |-> B(Initial), f |-> 0]]
    /\ nextF = 5
    /\ ops = [a \in Actors |-> 0]
    /\ crashes = 0
    /\ deaths = 0
    /\ serves = 0

\* An actor's locals once its verb has ended: nothing of a finished run is kept.
ResetLocals(a) ==
    /\ verb' = [verb EXCEPT ![a] = "place"]
    /\ tgt' = [tgt EXCEPT ![a] = 0]
    /\ ev' = [ev EXCEPT ![a] = 0]
    /\ wf' = [wf EXCEPT ![a] = 0]
    /\ np' = [np EXCEPT ![a] = 1]
    /\ frm' = [frm EXCEPT ![a] = 0]
    /\ ph' = [ph EXCEPT ![a] = 2]
    /\ cands' = [cands EXCEPT ![a] = {}]
ResetAll ==
    /\ verb' = [a \in Actors |-> "place"]
    /\ tgt' = [a \in Actors |-> 0]
    /\ ev' = [a \in Actors |-> 0]
    /\ wf' = [a \in Actors |-> 0]
    /\ np' = [a \in Actors |-> 1]
    /\ frm' = [a \in Actors |-> 0]
    /\ ph' = [a \in Actors |-> 2]
    /\ cands' = [a \in Actors |-> {}]

Files == <<fl, fd, sealed>>
Dir == <<dir, dirD, act, actD, pend>>
Disk == <<fl, fd, sealed, dir, dirD, act, actD, pend>>
Locals == <<verb, tgt, ev, wf, np, frm, ph, cands>>
Counts == <<nextF, ops, crashes, deaths, serves>>

\* An actor starts a verb: place tree t with the evidence of tree e (0: none), make t current, switch back, or prune.
Start(a, v, t, e) ==
    /\ pc[a] = "idle" /\ ops[a] < MaxOps
    /\ v \in {"place", "switch"} => t # 0
    /\ v # "place" => e = 0
    /\ v \in {"back", "prune"} => t = 0
    /\ pc' = [pc EXCEPT ![a] = "lock"]
    /\ verb' = [verb EXCEPT ![a] = v]
    /\ tgt' = [tgt EXCEPT ![a] = t]
    /\ ev' = [ev EXCEPT ![a] = e]
    /\ ops' = [ops EXCEPT ![a] = @ + 1]
    /\ UNCHANGED <<wf, np, frm, ph, cands, lock, pex, Disk, srv, nextF, crashes, deaths, serves>>

\* One try of the lock (flock LOCK_EX | LOCK_NB on the builds directory's descriptor): taken when free; held by another
\* actor, the try changes nothing and the code pauses and tries again. no_lock takes nothing.
LockTry(a) ==
    /\ pc[a] = "lock"
    /\ IF Bug = "no_lock"
       THEN /\ pc' = [pc EXCEPT ![a] = "settle"]
            /\ UNCHANGED lock
       ELSE IF lock = None
       THEN /\ lock' = a
            /\ pc' = [pc EXCEPT ![a] = "settle"]
       ELSE UNCHANGED <<pc, lock>>
    /\ UNCHANGED <<Locals, pex, Disk, srv, Counts>>

\* The wait for the lock ran out (another actor held it the whole time): exit 4, nothing done.
LockGiveUp(a) ==
    /\ pc[a] = "lock" /\ lock # None /\ lock # a
    /\ pc' = [pc EXCEPT ![a] = "idle"]
    /\ ResetLocals(a)
    /\ UNCHANGED <<lock, pex, Disk, srv, Counts>>

\* The builds directory synced (fsync on its descriptor): its names and the pointer survive a crash from now on.
Synced == /\ dirD' = dir /\ actD' = act /\ pend' = <<>>

\* With the lock: the directory synced first, so that what an actor that died left (a rename not yet synced) is
\* durable before the leftovers are judged against it.
Settle(a) ==
    /\ pc[a] = "settle"
    /\ Synced
    /\ pc' = [pc EXCEPT ![a] = IF HasLeftover THEN "clean" ELSE After(a)]
    /\ UNCHANGED <<Locals, lock, pex, Files, dir, act, srv, Counts>>

\* A leftover of an interrupted run removed (the lock is held, so its owner is dead): its contents, then its name.
\* (Without the lock another actor may have removed it first: the removal fails, and the run goes on with the rest.)
CleanOne(a) ==
    /\ pc[a] = "clean"
    /\ IF HasLeftover
       THEN \E n \in DOMAIN dir :
              /\ Leftover(n)
              /\ Change(Op(n, NoName, 0, NoAct))
              /\ fl' = [fl EXCEPT ![dir[n]] = Free]
              /\ fd' = [fd EXCEPT ![dir[n]] = Free]
              /\ pc' = [pc EXCEPT ![a] = IF \E m \in DOMAIN dir : Leftover(m) /\ m # n THEN "clean" ELSE After(a)]
       ELSE /\ pc' = [pc EXCEPT ![a] = After(a)]
            /\ UNCHANGED <<Dir, fl, fd>>
    /\ UNCHANGED <<Locals, lock, pex, sealed, dirD, actD, srv, Counts>>

\* place: the temporary folder made (mkdirat .tmp-*). reuse_tmp carries on with a half-placed one; in_place writes
\* into the folder of the build that is current.
MkTmp(a) ==
    /\ pc[a] = "mktmp"
    /\ IF Bug = "reuse_tmp" /\ \E n \in DOMAIN dir : n.k = "t"
       THEN \E n \in DOMAIN dir :
              /\ n.k = "t"
              /\ wf' = [wf EXCEPT ![a] = dir[n]]
              /\ np' = [np EXCEPT ![a] = Cardinality({i \in Parts : fl[dir[n]].parts[i] # 0}) + 1]
              /\ pc' = [pc EXCEPT ![a] = IF np'[a] > 3 THEN "manifest" ELSE "write"]
              /\ UNCHANGED <<fl, Dir, nextF>>
       ELSE IF Bug = "in_place"
       THEN /\ wf' = [wf EXCEPT ![a] = dir[B(Cur)]]
            /\ np' = [np EXCEPT ![a] = 1]
            /\ pc' = [pc EXCEPT ![a] = "write"]
            /\ UNCHANGED <<fl, Dir, nextF>>
       ELSE /\ nextF \in Fids
            /\ fl' = [fl EXCEPT ![nextF] = [Free EXCEPT !.kind = "build"]]
            /\ Change(Op(NoName, T(nextF), nextF, NoAct))
            /\ wf' = [wf EXCEPT ![a] = nextF]
            /\ np' = [np EXCEPT ![a] = 1]
            /\ nextF' = nextF + 1
            /\ pc' = [pc EXCEPT ![a] = "write"]
            /\ UNCHANGED <<dirD, actD>>
    /\ UNCHANGED <<verb, tgt, ev, frm, ph, cands, lock, pex, fd, sealed, srv, ops, crashes, deaths, serves>>

\* place: one part copied (every file of it written, checked, synced and closed; its directories made).
Write(a) ==
    /\ pc[a] = "write"
    /\ fl' = [fl EXCEPT ![wf[a]].parts[np[a]] = tgt[a]]
    /\ np' = [np EXCEPT ![a] = @ + 1]
    /\ pc' = [pc EXCEPT ![a] = IF np[a] = 3 THEN "manifest" ELSE "write"]
    /\ UNCHANGED <<verb, tgt, ev, wf, frm, ph, cands, lock, pex, fd, sealed, Dir, srv, Counts>>

\* place: the evidence checked against the hash of everything copied, and the manifest written. Evidence that names
\* another tree is refused (the folder is dropped); none is placed untested. tested_program_only takes evidence whose
\* program is this tree's (the driver left out of what it covers); stale_marker takes any evidence.
Accept(a) == CASE Bug = "tested_program_only" -> Prog(ev[a]) = Prog(tgt[a])
               [] Bug = "stale_marker" -> TRUE
               [] OTHER -> ev[a] = tgt[a]
Manifest(a) ==
    /\ pc[a] = "manifest"
    /\ IF ev[a] # 0 /\ ~Accept(a)
       THEN /\ pc' = [pc EXCEPT ![a] = "drop"]
            /\ UNCHANGED fl
       ELSE /\ fl' = [fl EXCEPT ![wf[a]].man = tgt[a], ![wf[a]].mark = (ev[a] # 0)]
            /\ pc' = [pc EXCEPT ![a] = IF Bug = "no_sync" THEN "verify" ELSE "sync"]
    /\ UNCHANGED <<Locals, lock, pex, fd, sealed, Dir, srv, Counts>>

\* place: every file and directory of the folder synced (F_FULLFSYNC on macOS), its modes made read-only first.
Sync(a) ==
    /\ pc[a] = "sync"
    /\ fd' = [fd EXCEPT ![wf[a]] = fl[wf[a]]]
    /\ pc' = [pc EXCEPT ![a] = "verify"]
    /\ UNCHANGED <<Locals, lock, pex, fl, sealed, Dir, srv, Counts>>

\* place: every file read back and checked against the manifest; a folder that does not match is dropped. The reused
\* or in-place folders of the broken builds are taken as they are.
Verify(a) ==
    /\ pc[a] = "verify"
    /\ pc' = [pc EXCEPT ![a] = IF Whole(wf[a], tgt[a]) \/ Bug \in {"reuse_tmp", "in_place"} THEN "rename" ELSE "drop"]
    /\ UNCHANGED <<Locals, lock, pex, Disk, srv, Counts>>

\* place: the one rename of the folder to its hash name; a build already placed under it is kept and this one dropped.
Rename(a) ==
    /\ pc[a] = "rename"
    /\ IF Bug = "in_place"
       THEN /\ pc' = [pc EXCEPT ![a] = "syncdir"]
            /\ UNCHANGED <<Dir, sealed>>
       ELSE IF B(tgt[a]) \in DOMAIN dir
       THEN /\ pc' = [pc EXCEPT ![a] = "drop"]
            /\ UNCHANGED <<Dir, sealed>>
       ELSE IF TmpOf(wf[a]) = {}
       THEN /\ pc' = [pc EXCEPT ![a] = "unlock"]    \* the folder is gone (only without the lock): the rename fails
            /\ UNCHANGED <<Dir, sealed>>
       ELSE \E n \in TmpOf(wf[a]) :
              /\ Change(Op(n, B(tgt[a]), wf[a], NoAct))
              /\ sealed' = [sealed EXCEPT ![wf[a]] = Content(fl[wf[a]])]
              /\ pc' = [pc EXCEPT ![a] = "syncdir"]
              /\ UNCHANGED <<dirD, actD>>
    /\ UNCHANGED <<Locals, lock, pex, fl, fd, srv, Counts>>

\* The builds directory synced, at the end of a step that needs its names durable.
DirSync(a, from, to) ==
    /\ pc[a] = from
    /\ Synced
    /\ pc' = [pc EXCEPT ![a] = to]
    /\ UNCHANGED <<Locals, lock, pex, Files, dir, act, srv, Counts>>

\* place: the temporary folder removed (refused evidence, a folder that did not verify, or a build already placed).
Drop(a) ==
    /\ pc[a] = "drop"
    /\ IF TmpOf(wf[a]) # {}
       THEN \E n \in TmpOf(wf[a]) :
              /\ Change(Op(n, NoName, 0, NoAct))
              /\ fl' = [fl EXCEPT ![wf[a]] = Free]
              /\ fd' = [fd EXCEPT ![wf[a]] = Free]
              /\ UNCHANGED <<dirD, actD>>
       ELSE UNCHANGED <<Dir, fl, fd>>
    /\ pc' = [pc EXCEPT ![a] = "unlock"]
    /\ UNCHANGED <<Locals, lock, pex, sealed, srv, Counts>>

Unlock(a) ==
    /\ pc[a] = "unlock"
    /\ lock' = IF lock = a THEN None ELSE lock
    /\ pc' = [pc EXCEPT ![a] = "idle"]
    /\ ResetLocals(a)
    /\ UNCHANGED <<pex, Disk, srv, Counts>>

\* current / back: the pointer read and the target checked (there, whole, tested; not already current). Refused:
\* the lock released, nothing changed. two_renames makes the switch two pointer changes (previous, then current).
Check(a) ==
    /\ pc[a] = "check"
    /\ LET h == IF verb[a] = "back" THEN Prev ELSE tgt[a]
           ok == h # 0 /\ h # Cur /\ BuildOK(h) /\ fl[dir[B(h)]].mark
       IN IF ok
          THEN /\ tgt' = [tgt EXCEPT ![a] = h]
               /\ frm' = [frm EXCEPT ![a] = Cur]
               /\ ph' = [ph EXCEPT ![a] = IF Bug = "two_renames" THEN 1 ELSE 2]
               /\ pc' = [pc EXCEPT ![a] = "pmk"]
          ELSE /\ pc' = [pc EXCEPT ![a] = "unlock"]
               /\ UNCHANGED <<tgt, frm, ph>>
    /\ UNCHANGED <<verb, ev, wf, np, cands, lock, pex, Disk, srv, Counts>>

\* The pair folder made under a temporary name, with its two links (current, previous).
PMk(a) ==
    /\ pc[a] = "pmk"
    /\ nextF \in Fids
    /\ fl' = [fl EXCEPT ![nextF] = [Free EXCEPT !.kind = "pair", !.c = PairOf(a).x, !.p = PairOf(a).y]]
    /\ Change(Op(NoName, Q(nextF), nextF, NoAct))
    /\ wf' = [wf EXCEPT ![a] = nextF]
    /\ nextF' = nextF + 1
    /\ pc' = [pc EXCEPT ![a] = IF Bug = "no_sync" THEN "prename" ELSE "psync"]
    /\ UNCHANGED <<verb, tgt, ev, np, frm, ph, cands, lock, pex, fd, sealed, dirD, actD, srv, ops, crashes, deaths,
                   serves>>

PSync(a) ==
    /\ pc[a] = "psync"
    /\ fd' = [fd EXCEPT ![wf[a]] = fl[wf[a]]]
    /\ pc' = [pc EXCEPT ![a] = "prename"]
    /\ UNCHANGED <<Locals, lock, pex, fl, sealed, Dir, srv, Counts>>

\* The pair renamed to its name, which names both builds. A pair of that name already there, or the folder gone (only
\* without the lock): the rename fails, and the run ends.
PRename(a) ==
    /\ pc[a] = "prename"
    /\ IF PairOf(a) \in DOMAIN dir \/ Q(wf[a]) \notin DOMAIN dir
       THEN /\ pc' = [pc EXCEPT ![a] = "unlock"]
            /\ UNCHANGED Dir
       ELSE /\ Change(Op(Q(wf[a]), PairOf(a), wf[a], NoAct))
            /\ pc' = [pc EXCEPT ![a] = IF Bug = "no_sync" THEN "anew" ELSE "pdsync"]
            /\ UNCHANGED <<dirD, actD>>
    /\ UNCHANGED <<Locals, lock, pex, Files, srv, Counts>>

\* The new pointer link made under a temporary name (symlinkat .active-* -> the pair's name).
ANew(a) ==
    /\ pc[a] = "anew"
    /\ Change(Op(NoName, N(wf[a]), wf[a], NoAct))
    /\ pc' = [pc EXCEPT ![a] = "arename"]
    /\ UNCHANGED <<Locals, lock, pex, Files, dirD, actD, srv, Counts>>

\* THE switch: one rename of the new link over `active`. g: the build that was current before the new current.
ARename(a) ==
    /\ pc[a] = "arename"
    /\ IF N(wf[a]) \notin DOMAIN dir
       THEN /\ pc' = [pc EXCEPT ![a] = "unlock"]    \* the link is gone (only without the lock): the rename fails
            /\ UNCHANGED <<Dir, ph>>
       ELSE /\ Change(Op(N(wf[a]), NoName, 0, [pn |-> PairOf(a), g |-> IF PairOf(a).x # Cur THEN Cur ELSE act.g]))
            /\ pc' = [pc EXCEPT ![a] = IF Bug = "no_sync" THEN (IF ph[a] = 1 THEN "pmk" ELSE "unlock") ELSE "async"]
            /\ ph' = [ph EXCEPT ![a] = IF Bug = "no_sync" THEN 2 ELSE @]
            /\ UNCHANGED <<dirD, actD>>
    /\ UNCHANGED <<verb, tgt, ev, wf, np, frm, cands, lock, pex, Files, srv, Counts>>

ASync(a) ==
    /\ pc[a] = "async"
    /\ Synced
    /\ pc' = [pc EXCEPT ![a] = IF ph[a] = 1 THEN "pmk" ELSE "unlock"]
    /\ ph' = [ph EXCEPT ![a] = 2]
    /\ UNCHANGED <<verb, tgt, ev, wf, np, frm, cands, lock, pex, Files, dir, act, srv, Counts>>

\* prune: the builds listed; every one but current and previous is a candidate (the walk prunes with --keep 0).
PList(a) ==
    /\ pc[a] = "plist"
    /\ LET c == {t \in Trees : B(t) \in DOMAIN dir /\ t \notin {Cur, Prev}} IN
       /\ cands' = [cands EXCEPT ![a] = c]
       /\ pc' = [pc EXCEPT ![a] = IF c = {} THEN "unlock" ELSE "ptry"]
    /\ UNCHANGED <<verb, tgt, ev, wf, np, frm, ph, lock, pex, Disk, srv, Counts>>

\* prune: the candidate's folder locked exclusive without waiting (flock LOCK_EX | LOCK_NB): refused, a server holds it
\* shared, and it is kept. prune_in_use takes no lock and goes on.
PTry(a) ==
    /\ pc[a] = "ptry"
    /\ \E t \in cands[a] :
        LET f == IF B(t) \in DOMAIN dir THEN dir[B(t)] ELSE 0
            rest == cands[a] \ {t}
        IN IF (f = 0) \/ ((f \in Held) /\ (Bug # "prune_in_use"))
           THEN /\ cands' = [cands EXCEPT ![a] = rest]
                /\ pc' = [pc EXCEPT ![a] = IF rest = {} THEN "unlock" ELSE "ptry"]
                /\ UNCHANGED <<pex, tgt, wf>>
           ELSE /\ pex' = [pex EXCEPT ![a] = IF Bug = "prune_in_use" THEN 0 ELSE f]
                /\ tgt' = [tgt EXCEPT ![a] = t]
                /\ wf' = [wf EXCEPT ![a] = f]
                /\ pc' = [pc EXCEPT ![a] = "pgone"]
                /\ UNCHANGED cands
    /\ UNCHANGED <<verb, ev, np, frm, ph, lock, Disk, srv, Counts>>

\* prune: the build renamed aside (.gone-*), out of its hash name.
PGone(a) ==
    /\ pc[a] = "pgone"
    /\ IF B(tgt[a]) \in DOMAIN dir
       THEN /\ Change(Op(B(tgt[a]), G(wf[a]), wf[a], NoAct))
            /\ pc' = [pc EXCEPT ![a] = "pgsync"]
            /\ UNCHANGED <<dirD, actD>>
       ELSE /\ pc' = [pc EXCEPT ![a] = "unlock"]    \* gone already (only without the lock): the rename fails
            /\ UNCHANGED Dir
    /\ UNCHANGED <<Locals, lock, pex, Files, srv, Counts>>

\* prune: the directory synced, and the folder's lock released.
PGSync(a) ==
    /\ pc[a] = "pgsync"
    /\ Synced
    /\ pex' = [pex EXCEPT ![a] = 0]
    /\ pc' = [pc EXCEPT ![a] = "premove"]
    /\ UNCHANGED <<Locals, lock, Files, dir, act, srv, Counts>>

\* prune: the folder's contents removed, then its name; and the next candidate.
PRemove(a) ==
    /\ pc[a] = "premove"
    /\ IF G(wf[a]) \in DOMAIN dir
       THEN Change(Op(G(wf[a]), NoName, 0, NoAct))
       ELSE UNCHANGED <<dir, act, pend>>
    /\ fl' = [fl EXCEPT ![wf[a]] = Free]
    /\ fd' = [fd EXCEPT ![wf[a]] = Free]
    /\ LET rest == cands[a] \ {tgt[a]} IN
       /\ cands' = [cands EXCEPT ![a] = rest]
       /\ pc' = [pc EXCEPT ![a] = IF rest = {} THEN "unlock" ELSE "ptry"]
    /\ UNCHANGED <<verb, tgt, ev, wf, np, frm, ph, lock, pex, sealed, dirD, actD, srv, Counts>>

\* A server started from a build: its program's path resolved to the build's folder (through current, or a build's
\* path given by hand), and the program run.
SResolve(s) ==
    /\ srv[s].st = "down" /\ serves < MaxServes
    /\ \E t \in Trees :
        /\ B(t) \in DOMAIN dir
        /\ srv' = [srv EXCEPT ![s] = [st |-> "resolved", n |-> B(t), f |-> 0]]
    /\ serves' = serves + 1
    /\ UNCHANGED <<pc, Locals, lock, pex, Disk, nextF, ops, crashes, deaths>>

\* The server opens its build's folder by name, takes its lock shared (refused while prune holds it exclusive) and
\* checks that the folder it holds is still the one under the name; else it does not start. (A build removed and placed
\* again between the run and the hold is held in its new folder: the same bytes, by its name.)
SHold(s) ==
    /\ srv[s].st = "resolved"
    /\ LET n == srv[s].n
           ok == n \in DOMAIN dir /\ \A a \in Actors : pex[a] # dir[n]
       IN srv' = [srv EXCEPT ![s].st = IF ok THEN "up" ELSE "down", ![s].f = IF ok THEN dir[n] ELSE 0]
    /\ UNCHANGED <<pc, Locals, lock, pex, Disk, Counts>>

SStop(s) ==
    /\ srv[s].st = "up"
    /\ srv' = [srv EXCEPT ![s].st = "down"]
    /\ UNCHANGED <<pc, Locals, lock, pex, Disk, Counts>>

\* An actor killed between two steps: the disk as it is; the OS releases its locks.
Die(a) ==
    /\ pc[a] # "idle" /\ deaths < MaxDeaths
    /\ pc' = [pc EXCEPT ![a] = "idle"]
    /\ lock' = IF lock = a THEN None ELSE lock
    /\ pex' = [pex EXCEPT ![a] = 0]
    /\ deaths' = deaths + 1
    /\ ResetLocals(a)
    /\ UNCHANGED <<Disk, srv, nextF, ops, crashes, serves>>

\* The machine crashes: every process ends; each folder's contents are as now or as last synced, and of the
\* directory's changes since its last sync, any subset survives, applied in order.
Crash ==
    /\ crashes < MaxCrashes
    /\ \E keep \in SUBSET (1..Len(pend)), keepF \in SUBSET {f \in Fids : fl[f] # fd[f]} :
        LET r == Replay(dirD, actD, Pick(pend, keep, 1))
            f == [x \in Fids |-> IF x \in keepF THEN fl[x] ELSE fd[x]]
        IN /\ dir' = r[1] /\ dirD' = r[1]
           /\ act' = r[2] /\ actD' = r[2]
           /\ pend' = <<>>
           /\ fl' = f /\ fd' = f
    /\ pc' = [a \in Actors |-> "idle"]
    /\ lock' = None
    /\ pex' = [a \in Actors |-> 0]
    /\ srv' = [s \in Servers |-> [srv[s] EXCEPT !.st = "down"]]
    /\ crashes' = crashes + 1
    /\ ResetAll
    /\ UNCHANGED <<sealed, nextF, ops, deaths, serves>>

Step(a) ==
    \/ LockTry(a) \/ LockGiveUp(a) \/ Settle(a) \/ CleanOne(a)
    \/ MkTmp(a) \/ Write(a) \/ Manifest(a) \/ Sync(a) \/ Verify(a) \/ Rename(a) \/ DirSync(a, "syncdir", "unlock")
    \/ Drop(a) \/ Unlock(a)
    \/ Check(a) \/ PMk(a) \/ PSync(a) \/ PRename(a) \/ DirSync(a, "pdsync", "anew") \/ ANew(a) \/ ARename(a)
    \/ ASync(a)
    \/ PList(a) \/ PTry(a) \/ PGone(a) \/ PGSync(a) \/ PRemove(a)

Done == /\ \A a \in Actors : pc[a] = "idle" /\ ops[a] = MaxOps
        /\ UNCHANGED vars

Next ==
    \/ \E a \in Actors : \/ \E v \in Verbs, t \in Trees \cup {0}, e \in Tested \cup {0} : Start(a, v, t, e)
                         \/ Step(a) \/ Die(a)
    \/ \E s \in Servers : SResolve(s) \/ SHold(s) \/ SStop(s)
    \/ Crash
    \/ Done

Spec == Init /\ [][Next]_vars

\* Fairness, in plain words: every actor that has begun a verb takes its own next step (the code runs; the only thing
\* it waits on is the lock, whose holder steps too). Not assumed: a verb begun, a death, a crash, a server's start or
\* stop.
Live == Spec /\ \A a \in Actors : WF_vars(Step(a))

-----------------------------------------------------------------------------
TypeOK ==
    /\ pc \in [Actors -> {"idle", "lock", "settle", "clean", "mktmp", "write", "manifest", "sync", "verify", "rename",
                          "syncdir", "drop", "unlock", "check", "pmk", "psync", "prename", "pdsync", "anew",
                          "arename", "async", "plist", "ptry", "pgone", "pgsync", "premove"}]
    /\ lock \in Actors \cup {None}
    /\ \A f \in Fids : fl[f].kind \in {"free", "build", "pair"}

\* At every instant, crash or not: current names a whole, tested build (one is current from the start).
CurrentValid == Cur # 0 /\ BuildOK(Cur) /\ Cur \in Tested
\* previous names a whole build, or nothing.
PreviousValid == Prev # 0 => BuildOK(Prev)
\* The pointer's pair is there, and its links are the builds its name says.
PointerResolves == /\ act.pn \in DOMAIN dir
                   /\ fl[dir[act.pn]].kind = "pair"
                   /\ fl[dir[act.pn]].c = Cur /\ fl[dir[act.pn]].p = Prev
\* previous is the build that was current before the current one: the last good build is never lost from the pair.
PrevIsLastCurrent == Prev = act.g
\* A folder under its hash name is whole: by construction complete.
NamedWhole == \A t \in Trees : B(t) \in DOMAIN dir => Whole(dir[B(t)], t)
\* No build folder is ever modified after it appears under its hash name.
SealedUnchanged == \A t \in Trees : B(t) \in DOMAIN dir => Content(fl[dir[B(t)]]) = sealed[dir[B(t)]]
\* A build a running server holds is never removed or altered.
InUseKept == \A f \in Held : fl[f].kind = "build" /\ sealed[f] # NoContent /\ Content(fl[f]) = sealed[f]
\* At most one actor changes the pointer at a time.
OneSwitcher == Cardinality({a \in Actors : pc[a] \in SwitchPcs}) <= 1
\* Every step past the lock is taken by its holder.
LockHeld == \A a \in Actors : pc[a] \notin {"idle", "lock"} => lock = a

\* An interrupted placement never blocks a later one: every verb begun ends (done, refused, or busy), under Live.
VerbsEnd == \A a \in Actors : (pc[a] # "idle") ~> (pc[a] = "idle")
=============================================================================

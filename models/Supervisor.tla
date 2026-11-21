----------------------------- MODULE Supervisor -----------------------------
(***************************************************************************)
(* The driver supervisor (src/server/supervisor.h): what nexus-server does *)
(* with one session's driver process. It starts a driver when a prompt    *)
(* waits and none is alive, writes each prompt to it once as the next turn *)
(* number, gathers its replies into the tab, stops a turn, and ends a      *)
(* driver that breaks the protocol, goes silent where a deadline applies,  *)
(* or exits.                                                              *)
(*                                                                         *)
(* The driver is the environment: at any step it may write any message,    *)
(* for any turn near the open one, a line the server refuses, close its     *)
(* output, or exit. Time is abstract: Timeout is a deadline passing, and is *)
(* enabled only where the supervisor keeps one (waiting for hello, for a    *)
(* stop's done, and the grace periods while ending a driver). There is no   *)
(* deadline on a working turn.                                             *)
(*                                                                         *)
(* The person also queues prompts for later (Command-Enter): the tab's     *)
(* queue, apart from the transcript. When no turn is open, no prompt waits  *)
(* and starting is not held back, the oldest queued prompt leaves the queue *)
(* and joins the waiting prompts (shown as the person's entry): one per     *)
(* turn, oldest first, each once.                                          *)
(*                                                                         *)
(* The supervisor's state is one record, s. Its fields next, open, waiting, *)
(* queue and session are durable (the store): each step's effects and durable *)
(* state are committed in one store step before its commands are carried   *)
(* out, so a step here is atomic, and Restart (the server ended, by a      *)
(* crash or a stop, and started again) keeps exactly those fields. The     *)
(* driver of a server that crashed reads the end of its input and exits    *)
(* (the protocol): it is gone after Restart.                              *)
(*                                                                         *)
(* Ghosts, for the properties: written (every send written, [p, t]), fate  *)
(* (what became of each prompt), fx (the entries the last step appended to *)
(* the tab, each [k, t, p]: "P" the person's prompt p, "F" the friend's    *)
(* text of turn t, "T" a tool line, "E" an error notice of the turn, "K"   *)
(* the turn stopped, "X" the turn cut short, "N" any other notice) and     *)
(* resumed (the --resume the last start was given, "" for none), stops    *)
(* (the turn of every stop written to a driver, in order) and asked (the  *)
(* step was the person's stop).                                            *)
(*                                                                         *)
(* A stop ends what runs when it arrives: the open turn (a stop written to *)
(* the driver), or, while the driver starts, the turn about to begin (its  *)
(* prompt leaves the waiting prompts, never written). One that finds the   *)
(* turn already stopping, or nothing running, changes nothing: no stop is  *)
(* kept for a later turn.                                                  *)
(*                                                                         *)
(* The session's model and effort (its setting, a record of the two; ""   *)
(* is the harness's default) and its account are the person's to change   *)
(* at any time (SetSetting, SetAccount). A driver starts on the harness's  *)
(* defaults; told is what it has been told since. Before a send, a         *)
(* settings line gives it each field where the session's differs, which it *)
(* applies from that send on (docs/driver-protocol.md): so a change        *)
(* applies from the next turn and never to a turn in progress. A settings  *)
(* line cannot put a field back to the default (an empty field stays as it *)
(* is), so a driver told a field the session has since cleared is ended,   *)
(* as is one whose account is no longer the session's: once no turn is     *)
(* open, quietly, never counted as a failed start, and the next prompt     *)
(* starts one on the defaults under the session's account. The ghost drv   *)
(* is the account the driver alive was started under; each send written   *)
(* records the setting the driver runs it with.                            *)
(*                                                                         *)
(* The session's place is its friend's instance: a directory (dir) and     *)
(* whether it is on this machine (here), changed by edits (SetPlace). A    *)
(* driver started in another directory (the ghost ddir), or any driver     *)
(* while the instance is elsewhere, is ended like one under an old account; *)
(* while elsewhere no driver starts and nothing queued is taken. The        *)
(* harness session id is durable with its directory (sdir) and account    *)
(* (owner). A start in another directory or under another account drops   *)
(* it and starts afresh, owing (summary, durable) the conversation's       *)
(* summary to the first send, which carries it (each send's sum).          *)
(*                                                                         *)
(* The session's friend may be taken out of friends.json by a reload       *)
(* (RemoveFriend) and put back (AddBack); absent says it is out. Taken     *)
(* out, what runs is stopped as the person's stop stops it (a stop written *)
(* for the open turn, which ends at its done or is cut at the stop's       *)
(* deadline) and the driver is ended: a starting one at once, quietly, an  *)
(* idle one by Retire, a stopping one once its turn ends. While it is out  *)
(* no driver starts, no prompt is sent (the server refuses it first) and   *)
(* nothing queued is taken: the waiting prompts and the queue are kept,    *)
(* across restarts too, and go on once it is back. rm counts the removals  *)
(* (MaxRemovals bounds them) and the ghost removing marks the step.         *)
(*                                                                         *)
(* A server may also start holding no friends list at all: its committed  *)
(* files do not read (RestartUnknown). Then whether the friend is in it is *)
(* unknown (known is FALSE), and unknown reads as out: absent, so nothing  *)
(* runs, nothing is sent and nothing queued is taken, and the prompts are  *)
(* kept. The first list applied after that (Learn) says, from itself      *)
(* alone, whether the friend is in or out: it is never diffed against an  *)
(* empty list that would read as "it was out, and is back".                *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Prompts,          \* how many prompts the person sends, numbered 1..Prompts
    MaxWaiting,       \* how many prompts may wait (the hub's queue limit)
    MaxFailedStarts,  \* consecutive failed starts before the supervisor stops starting drivers
    MaxRestarts,      \* how many times the server restarts in a behaviour
    Sessions,         \* the harness session ids a driver may report
    Models,           \* the models the person may choose, besides "" (the harness's default)
    Efforts,          \* the efforts the person may choose, besides ""
    Accounts,         \* the accounts the person may choose ("" is the default)
    Dirs,             \* the directories the session's instance may move to ("" is where it starts)
    MaxSwitches,      \* how many times the person changes the setting, the account or the place, in all
    MaxRemovals       \* how many times the session's friend is taken out of friends.json

ASSUME MaxWaiting >= 1 /\ MaxFailedStarts >= 1

VARIABLES
    s,          \* the supervisor, with its ghosts
    sent,       \* how many prompts the person has sent
    restarts,   \* how many times the server restarted
    switches    \* how many times the person changed the setting or the account

vars == <<s, sent, restarts, switches>>

Settings == [m : Models \cup {""}, e : Efforts \cup {""}]
Default == [m |-> "", e |-> ""]

Phases == {"none", "starting", "idle", "working", "stopping", "ending"}
Stages == {"none", "closed", "termed", "killed"}   \* while ending: input closed, then TERM sent, then KILL sent
Fates == {"unsent", "refused", "queued", "waiting", "open", "answered", "cut"}
Kinds == {"P", "F", "T", "E", "K", "X", "N"}
TurnKinds == {"F", "T", "E", "K", "X"}
Billings == {"subscription", "api-key", "none"}
Outcomes == {"ok", "stopped", "error"}
E(k, t, p) == [k |-> k, t |-> t, p |-> p]

Init ==
    /\ sent = 0
    /\ restarts = 0
    /\ switches = 0
    /\ s = [phase |-> "none", stage |-> "none", open |-> 0, prompt |-> 0, next |-> 1, waiting |-> <<>>,
            queue |-> <<>>, session |-> "", gathered |-> FALSE, finished |-> FALSE, failed |-> 0, blocked |-> FALSE,
            nocred |-> FALSE, down |-> FALSE, crashed |-> FALSE, procs |-> 0, written |-> <<>>, stops |-> <<>>,
            fate |-> [p \in 1 .. Prompts |-> "unsent"], fx |-> <<>>, resumed |-> "", asked |-> FALSE, signin |-> FALSE,
            setting |-> Default, told |-> Default, account |-> "", drv |-> "", retiring |-> FALSE,
            unconf |-> FALSE, dir |-> "", here |-> TRUE, ddir |-> "", sdir |-> "", owner |-> "", summary |-> FALSE,
            lim |-> FALSE, dlim |-> FALSE, absent |-> FALSE, rm |-> 0, removing |-> FALSE, known |-> TRUE]

----------------------------------------------------------------------------
\* The parts of a step, each from one supervisor record to the next.

Fx(x, e) == [x EXCEPT !.fx = Append(@, e)]

\* The text gathered for the open turn goes into the tab as the friend's entry.
Flush(x) == IF x.gathered THEN [Fx(x, E("F", x.open, 0)) EXCEPT !.gathered = FALSE] ELSE x

\* The open turn ends without its done: a notice that it was cut short. The prompt is never written again.
CutOpen(x) == [Fx(x, E("X", x.open, 0)) EXCEPT !.fate[x.prompt] = "cut", !.open = 0, !.prompt = 0]

\* The driver is gone or going: the text gathered, a notice of why, and the open turn cut. It failed (crashed): the
\* friend shows down until a driver says hello again (Activity).
Abandon(x) ==
    LET y == [Fx(Flush(x), E("N", 0, 0)) EXCEPT !.crashed = TRUE] IN
    IF y.open # 0 THEN CutOpen(y) ELSE y

\* End the driver with no turn open: close its input; TERM and KILL follow on deadlines.
QuietEnd(x) == [x EXCEPT !.phase = "ending", !.stage = "closed"]

\* End the driver: abandon it and close its input.
End(x) == QuietEnd(Abandon(x))

\* The open turn's done. A driver that said it has no credential exits next: it is ended, never given a prompt.
Close(x) ==
    LET y == [x EXCEPT !.fate[x.prompt] = "answered", !.open = 0, !.prompt = 0, !.phase = "idle",
                       !.finished = TRUE, !.failed = 0]
    IN IF x.nocred THEN QuietEnd(y) ELSE y

\* Write the oldest waiting prompt to an idle driver as the next turn. V(x) is what the driver runs it with: what it
\* was told, with each non-empty field of the settings line written first (none when nothing differs).
WriteNextWith(V(_), x) ==
    IF x.phase = "idle" /\ x.waiting # <<>>
    THEN LET p == Head(x.waiting) IN
         [x EXCEPT !.open = x.next, !.prompt = p, !.next = x.next + 1, !.waiting = Tail(x.waiting),
                   !.written = Append(@, [p |-> p, t |-> x.next, v |-> V(x), sum |-> x.summary]), !.fate[p] = "open",
                   !.phase = "working", !.told = V(x), !.summary = FALSE]
    ELSE x
Told(x) == [m |-> IF x.setting.m # "" THEN x.setting.m ELSE x.told.m,
            e |-> IF x.setting.e # "" THEN x.setting.e ELSE x.told.e]
WriteNext(x) == WriteNextWith(Told, x)

\* Start a driver when a prompt waits and none is alive and the instance is here, in the session's directory, under
\* the session's account, with --resume the last ID reported by that account in that directory. An incompatible ID
\* is dropped (D), and when the conversation has turns the first send owes a summary, said in a notice. With no driver
\* configured the start names no program, and so nothing to resume: it fails, as the exit that follows says.
Drop(x) ==
    IF x.session # "" /\ (x.sdir # x.dir \/ x.owner # x.account)
    THEN [(IF x.next > 1 THEN Fx(x, E("N", 0, 0)) ELSE x) EXCEPT !.session = "", !.sdir = "", !.owner = "",
                                                                 !.summary = @ \/ x.next > 1]
    ELSE x
StartDriverWith(D(_), x) ==
    IF x.phase = "none" /\ x.waiting # <<>> /\ ~x.blocked /\ ~x.down /\ x.here /\ ~x.absent
    THEN LET y == D(x) IN
         [y EXCEPT !.phase = "starting", !.procs = @ + 1, !.finished = FALSE, !.nocred = FALSE,
                   !.resumed = IF y.unconf THEN "" ELSE y.session, !.drv = y.account, !.told = Default,
                   !.retiring = FALSE, !.ddir = y.dir, !.dlim = y.lim]
    ELSE x
StartDriver(x) == StartDriverWith(Drop, x)

\* With no turn open, a driver started under an account that is no longer the session's, or in a directory that is no
\* longer the session's, or any while the instance is elsewhere, or one told a field the session has put back to the
\* default, is ended quietly: the next one starts on the defaults under the session's account, in its directory. So is
\* any while the friend is out of friends.json.
Cleared(x) == (x.told.m # "" /\ x.setting.m = "") \/ (x.told.e # "" /\ x.setting.e = "")
Moved(x) == x.ddir # x.dir \/ ~x.here
\* A driver started under another limit than the session's (Supervisor::retire's driverLimited_ check).
OtherLimit(x) == x.dlim # x.lim
RetireAll(C(_), L(_), x) ==
    IF x.phase = "idle" /\ (x.drv # x.account \/ C(x) \/ Moved(x) \/ L(x) \/ x.absent)
    THEN [QuietEnd(x) EXCEPT !.retiring = TRUE] ELSE x
RetireWith(C(_), x) == RetireAll(C, OtherLimit, x)
Retire(x) == RetireWith(Cleared, x)

\* When a driver is configured, the friend is in friends.json, no turn is open, no prompt waits and starting is not
\* held back: the oldest queued prompt joins the waiting prompts, shown as the person's entry. One per turn: it waits,
\* or is written as the turn that opens.
Take(x) ==
    IF ~x.unconf /\ x.here /\ ~x.absent /\ x.open = 0 /\ x.waiting = <<>> /\ ~x.blocked /\ ~x.down /\ x.queue # <<>>
    THEN LET p == Head(x.queue) IN
         [Fx(x, E("P", 0, p)) EXCEPT !.queue = Tail(@), !.waiting = <<p>>, !.fate[p] = "waiting"]
    ELSE x

\* What follows every step. The witnesses break a part through AdvanceAll.
AdvanceAll(Tk(_), Rt(_), Wn(_), x) == Wn(StartDriver(Rt(Tk(x))))
AdvanceWith(Tk(_), x) == AdvanceAll(Tk, Retire, WriteNext, x)
Advance(x) == AdvanceWith(Take, x)

\* Every step starts with no effects, and is not a stop, a sign-in or a removal until it says so.
Fresh == [s EXCEPT !.fx = <<>>, !.asked = FALSE, !.signin = FALSE, !.removing = FALSE]

\* The driver is gone: a failed start is one that finished no turn (and was not ended for an account change).
ExitedWith(x) ==
    LET y == IF x.phase = "ending" THEN x ELSE Abandon(x)
        count == ~y.finished /\ ~y.blocked /\ ~y.down /\ ~y.retiring
        f == IF count THEN y.failed + 1 ELSE y.failed
        z == [y EXCEPT !.phase = "none", !.stage = "none", !.procs = @ - 1, !.failed = f, !.finished = FALSE,
                       !.nocred = FALSE, !.gathered = FALSE, !.drv = "", !.told = Default, !.retiring = FALSE,
                       !.ddir = "", !.dlim = FALSE]
    IN IF count /\ f >= MaxFailedStarts THEN [Fx(z, E("N", 0, 0)) EXCEPT !.blocked = TRUE] ELSE z

\* An error of code no-credential (v): the driver ends the open turn, if any, and exits (protocol rule 11). No
\* driver is started until the person's next prompt, and this one is ended once no turn is open.
NoCredential(m, x) ==
    IF m.v # "no-credential" THEN x
    ELSE LET y == [x EXCEPT !.blocked = TRUE, !.nocred = TRUE] IN
         IF y.phase = "idle" THEN QuietEnd(y) ELSE y

\* A message from the driver.
ReceiveWith(x, m, Txt(_, _)) ==
    CASE x.phase = "ending"   -> x
      [] x.phase = "starting" -> IF m.kind = "hello" /\ (m.v = "1" \/ (m.v = "1n" /\ ~x.dlim))
                                 THEN [x EXCEPT !.phase = "idle", !.crashed = FALSE]
                                 ELSE End(x)
      [] m.kind = "hello"      -> End(x)
      [] m.kind = "credential" -> IF m.v = "subscription" THEN x ELSE [End(x) EXCEPT !.blocked = TRUE]
      [] m.kind = "session"    -> [x EXCEPT !.session = m.v, !.sdir = x.ddir, !.owner = x.drv]
      [] m.kind = "error"      -> NoCredential(m, IF m.t = 0 THEN Fx(x, E("N", 0, 0))
                                                 ELSE IF m.t = x.open THEN Fx(x, E("E", m.t, 0)) ELSE x)
      [] m.kind = "text"       -> Txt(x, m)
      [] m.t # x.open          -> End(x)
      [] m.kind = "text-end"   -> Flush(x)
      [] m.kind = "tool"       -> Fx(Flush(x), E("T", x.open, 0))
      [] m.kind = "done"       -> Close(IF m.v = "stopped" THEN Fx(Flush(x), E("K", x.open, 0)) ELSE Flush(x))
      [] OTHER                 -> x

\* text gathers for the open turn; for any other turn it breaks the protocol.
Gather(x, m) == IF m.t = x.open /\ x.open # 0 THEN [x EXCEPT !.gathered = TRUE] ELSE End(x)

\* Whether an instance takes changes of the session's limit (SetLimit), as SignIns does for sign-ins: the instances
\* that spend their changes on something else replace it with MCSupervisor's NoLimitChanges. With it, a driver may
\* also say hello without the "limit" feature ("1n"), and one started limited that does is ended.
LimitChanges == TRUE

\* The messages a driver may write here: every kind, for turns about the open one.
Turns(x) == {t \in {x.open - 1, x.open, x.open + 1, x.next - 1, x.next} : t >= 1}
Messages(x) ==
    {[kind |-> "hello", t |-> 0, v |-> v] : v \in {"1", "2"} \cup (IF LimitChanges THEN {"1n"} ELSE {})} \cup
    {[kind |-> "credential", t |-> 0, v |-> b] : b \in Billings} \cup
    {[kind |-> "session", t |-> 0, v |-> i] : i \in Sessions} \cup
    {[kind |-> k, t |-> t, v |-> ""] : k \in {"text", "text-end", "tool"}, t \in Turns(x)} \cup
    {[kind |-> "done", t |-> t, v |-> o] : t \in Turns(x), o \in Outcomes} \cup
    {[kind |-> "error", t |-> t, v |-> c] : t \in Turns(x) \cup {0}, c \in {"", "no-credential"}}

----------------------------------------------------------------------------
\* The steps. Each witness passes a broken part through the With forms.

Keep == UNCHANGED <<sent, restarts, switches>>

\* The person sends prompt p: refused with a notice past the bound; otherwise it shows, waits, and goes out when it can.
\* A prompt also lifts the hold that failed starts put on starting drivers. With no driver configured the server
\* refuses it before it reaches the supervisor (Drivers::send): no step here; so it does while the friend is out of
\* friends.json.
PromptWith(Adv(_)) ==
    /\ ~s.down
    /\ ~s.unconf
    /\ ~s.absent
    /\ sent < Prompts
    /\ sent' = sent + 1
    /\ UNCHANGED <<restarts, switches>>
    /\ LET p == sent + 1
           x == Fresh
       IN s' = IF Len(x.waiting) >= MaxWaiting
               THEN [Fx(x, E("N", 0, 0)) EXCEPT !.fate[p] = "refused"]
               ELSE Adv([Fx(x, E("P", 0, p)) EXCEPT !.waiting = Append(@, p), !.fate[p] = "waiting",
                                                     !.blocked = FALSE, !.failed = IF x.blocked THEN 0 ELSE @])
Prompt == PromptWith(Advance)

\* The person queues prompt p for later. The hub refuses one past its limit before it reaches the supervisor, so the
\* queue holds at most MaxWaiting (the code's limit for both). Like a prompt, it lifts the hold on starting drivers.
QueuePromptWith(Adv(_)) ==
    /\ ~s.down
    /\ sent < Prompts
    /\ Len(s.queue) < MaxWaiting
    /\ sent' = sent + 1
    /\ UNCHANGED <<restarts, switches>>
    /\ LET p == sent + 1
           x == Fresh
       IN s' = Adv([x EXCEPT !.queue = Append(@, p), !.fate[p] = "queued", !.blocked = FALSE,
                             !.failed = IF x.blocked THEN 0 ELSE @])
QueuePrompt == QueuePromptWith(Advance)

\* The person stops the friend's work: a stop for the open turn, if one is open and no stop is out; while the driver
\* starts, the turn about to begin is cancelled (C). It never waits: it acts in the step it is given, or not at all.
\* The stop of the open turn, written once (Supervisor::stopOpen).
StopOpen(x) == [x EXCEPT !.phase = "stopping", !.stops = Append(@, x.open)]
StopWith(Ok(_), C(_)) ==
    /\ Keep
    /\ s' = LET x == [Fresh EXCEPT !.asked = TRUE] IN
            CASE Ok(x)                                     -> StopOpen(x)
              [] x.phase = "starting" /\ x.waiting # <<>> -> C(x)
              [] OTHER                                     -> x
Working(x) == x.phase = "working"
\* The prompt the starting driver was to be given leaves the waiting prompts, stopped: it is never written.
CancelStart(x) == [Fx(x, E("K", 0, 0)) EXCEPT !.waiting = Tail(@), !.fate[Head(x.waiting)] = "cut"]
Stop == StopWith(Working, CancelStart)

\* Each step that ends with what follows every step takes that as A, so that the take's witnesses can break it
\* everywhere (NextTake); the plain forms pass Advance.
MessageA(m, Txt(_, _), A(_)) ==
    /\ s.procs > 0 /\ s.phase # "none"
    /\ Keep
    /\ s' = A(ReceiveWith(Fresh, m, Txt))
MessageWith(m, Txt(_, _)) == MessageA(m, Txt, Advance)
Message(m) == MessageWith(m, Gather)

\* A line the server refuses, or the driver's output or input closing: the driver is ended. The turn it cuts is a
\* turn's end: the next queued prompt may go.
BrokenA(A(_)) ==
    /\ s.procs > 0 /\ s.phase \notin {"none", "ending"}
    /\ Keep
    /\ s' = A(End(Fresh))
Broken == BrokenA(Advance)

\* The driver exits, by itself or by a signal.
ExitA(Ex(_), A(_)) ==
    /\ s.procs > 0 /\ s.phase # "none"
    /\ Keep
    /\ s' = A(Ex(Fresh))
ExitWith(Ex(_)) == ExitA(Ex, Advance)
Exit == ExitWith(ExitedWith)

\* A deadline passes: no hello, no done after a stop, or a grace period while ending.
TimeoutA(Starting(_), Stopping(_), A(_)) ==
    /\ \/ s.phase \in {"starting", "stopping"}
       \/ s.phase = "ending" /\ s.stage # "killed"
    /\ Keep
    /\ s' = LET x == Fresh IN
            A(CASE x.phase = "starting"                       -> Starting(x)
                      [] x.phase = "stopping"                       -> Stopping(x)
                      [] x.phase = "ending" /\ x.stage = "closed"   -> [x EXCEPT !.stage = "termed"]
                      [] OTHER                                      -> [x EXCEPT !.stage = "killed"])
TimeoutWith(Starting(_), Stopping(_)) == TimeoutA(Starting, Stopping, Advance)
Timeout == TimeoutWith(End, End)

\* The server stops: every driver is ended, and none is started again. A driver with no turn open is ended quietly.
Shutdown ==
    /\ ~s.down
    /\ Keep
    /\ s' = LET x == [Fresh EXCEPT !.down = TRUE] IN
            CASE x.phase \in {"none", "ending"} -> x
              [] x.open # 0                     -> End(x)
              [] OTHER                          -> QuietEnd(x)

\* The server starts again from what is durable. A turn left open is cut; waiting prompts go to a new driver. It reads
\* its committed friends list: the friend is in it or out as before, but after a start that held none (known FALSE),
\* when the list may say either (v).
RestartA(R(_), A(_)) ==
    /\ restarts < MaxRestarts
    /\ restarts' = restarts + 1
    /\ UNCHANGED <<sent, switches>>
    /\ \E v \in BOOLEAN : s' = A(R([Fresh EXCEPT !.known = TRUE, !.absent = IF s.known THEN @ ELSE v]))
RestartWith(R(_)) == RestartA(R, Advance)
Restarted(x) ==
    LET y == IF x.open # 0 THEN CutOpen(x) ELSE x IN
    [y EXCEPT !.phase = "none", !.stage = "none", !.gathered = FALSE, !.finished = FALSE, !.failed = 0,
              !.blocked = FALSE, !.nocred = FALSE, !.down = FALSE, !.procs = 0, !.drv = "", !.told = Default,
              !.retiring = FALSE, !.crashed = FALSE, !.ddir = "", !.dlim = FALSE]
Restart == RestartWith(Restarted)

\* The server starts again and its committed friends list does not read (server.cpp restoreAll: Hub::keepWith), so it
\* holds none: the friend is not known to be in it, and reads out (Drivers::listed is false with no list) until one is
\* applied. Ab is what it reads as: TRUE, out; the witness passes FALSE. Only where the friend may be taken out at all
\* (MaxRemovals > 0), so that the other instances keep their state spaces.
RestartUnknownWith(Ab) ==
    /\ MaxRemovals > 0
    /\ restarts < MaxRestarts
    /\ restarts' = restarts + 1
    /\ UNCHANGED <<sent, switches>>
    /\ s' = Advance(Restarted([Fresh EXCEPT !.known = FALSE, !.absent = Ab]))
RestartUnknown == RestartUnknownWith(TRUE)

\* The first friends list applied after a start that held none (Hub::applyAccounts from no registry): the friend is in
\* it (v) or not, from that list alone, and the supervisor is told so (Drivers::present); what waits goes on if it is in.
Learn(v) ==
    /\ ~s.known
    /\ Keep
    /\ s' = Advance([Fresh EXCEPT !.known = TRUE, !.absent = ~v])

\* The person sets the session's model and effort (v): the next send is written with it, and an idle driver told a
\* field v clears is ended (Retire; with a turn open, the Advance at its end does it). Nothing else happens.
SetSettingWith(St(_, _), v) ==
    /\ switches < MaxSwitches
    /\ switches' = switches + 1
    /\ UNCHANGED <<sent, restarts>>
    /\ s' = St(Fresh, v)
SettingSet(x, v) == Retire([x EXCEPT !.setting = v])
SetSetting(v) == SetSettingWith(SettingSet, v)

\* The person sets the session's account (a). A driver started under another is ended once no turn is open (Advance).
SetAccountWith(Ac(_, _), a) ==
    /\ switches < MaxSwitches
    /\ switches' = switches + 1
    /\ UNCHANGED <<sent, restarts>>
    /\ s' = Ac(Fresh, a)
AccountSet(x, a) == Advance([x EXCEPT !.account = a])
SetAccount(a) == SetAccountWith(AccountSet, a)

\* The session's instance changed (an edit): its directory is d, and h says whether it is on this machine. A driver
\* started elsewhere is ended once no turn is open (Advance), and the next starts in d (Drop decides whether it
\* resumes).
SetPlaceWith(Pl(_, _, _), d, h) ==
    /\ switches < MaxSwitches
    /\ switches' = switches + 1
    /\ UNCHANGED <<sent, restarts>>
    /\ s' = Pl(Fresh, d, h)
PlaceSet(x, d, h) == Advance([x EXCEPT !.dir = d, !.here = h])
SetPlace(d, h) == SetPlaceWith(PlaceSet, d, h)

\* The drivers file loses the friend's driver (c), or gives it back: the next start runs what it names, and with
\* one again, what waits for it goes on (Supervisor::configure). It counts as a change of the person's.
Reconfigure(c) ==
    /\ switches < MaxSwitches
    /\ c # s.unconf
    /\ switches' = switches + 1
    /\ UNCHANGED <<sent, restarts>>
    /\ s' = Advance([Fresh EXCEPT !.unconf = c])

\* The session's account signed in while starting is held back: its login arrived through the secret dialog
\* (docs/secrets.md, Drivers::signedIn). Like a prompt, it lifts the hold, without a restart, so what waits goes on
\* and the oldest queued prompt is taken. It counts as a change of the person's (they pasted the login).
\* SignIns says whether an instance takes it: TRUE here; the per-commit switch and place instances replace it with
\* MCSupervisor's NoSignIns, so that their one change stays a setting, an account or a place, and
\* MCSupervisorSignIn.cfg spends its one change on a sign-in instead.
SignIns == TRUE
SignIn ==
    /\ SignIns
    /\ s.blocked
    /\ switches < MaxSwitches
    /\ switches' = switches + 1
    /\ UNCHANGED <<sent, restarts>>
    /\ s' = Advance([Fresh EXCEPT !.blocked = FALSE, !.failed = 0, !.signin = TRUE])

\* The session's limit changed (lim: its friend is limited to its directory, from the files; Supervisor::setLimited).
\* Tightened while a turn runs on a driver started without the limit (dlim, the ghost of `--limit cwd`), that turn is
\* stopped at once, as the person's stop does, with a notice; the driver is then ended once no turn is open (Retire's
\* OtherLimit) and the next starts limited. Loosened, the open turn finishes as it started. It counts as a change.
\* LimitChanges (defined with the messages) says whether an instance takes it.
SetLimitWith(Sl(_, _), v) ==
    /\ LimitChanges
    /\ v # s.lim
    /\ switches < MaxSwitches
    /\ switches' = switches + 1
    /\ UNCHANGED <<sent, restarts>>
    /\ s' = Sl(Fresh, v)
LimitSet(x, v) ==
    LET y == [x EXCEPT !.lim = v] IN
    Advance(IF v /\ ~y.dlim /\ y.phase = "working"
            THEN [Fx(y, E("N", 0, 0)) EXCEPT !.phase = "stopping", !.stops = Append(@, y.open)]
            ELSE y)
SetLimit(v) == SetLimitWith(LimitSet, v)

\* The session's friend is taken out of friends.json (a reload; Supervisor::setPresent). Rm stops what runs: the open
\* turn gets the person's stop (StopOpen), and a starting driver, with no turn open, is ended at once, never counted as
\* a failed start; then what follows every step, whose Retire ends an idle driver and whose Take and StartDriver do
\* nothing while the friend is out. The waiting prompts and the queue are kept. The limit is a change of its own
\* (SetLimit): a friend out of the files reads limited (registry.h limitedFor), and one put back gets its record's.
RemoveFriendA(Rm(_), A(_)) ==
    /\ s.known
    /\ ~s.absent
    /\ s.rm < MaxRemovals
    /\ Keep
    /\ s' = A(Rm([Fresh EXCEPT !.absent = TRUE, !.rm = @ + 1, !.removing = TRUE]))
Removed(x) ==
    CASE Working(x)              -> StopOpen(x)
      [] x.phase = "starting"    -> [QuietEnd(x) EXCEPT !.retiring = TRUE]
      [] OTHER                   -> x
RemoveFriendWith(Rm(_)) == RemoveFriendA(Rm, Advance)
RemoveFriend == RemoveFriendWith(Removed)

\* The friend is put back in friends.json: what waits goes on (the oldest queued prompt is taken, a driver started). A
\* list applied while none is held is Learn's, not this.
AddBackA(A(_)) ==
    /\ s.known
    /\ s.absent
    /\ Keep
    /\ s' = A([Fresh EXCEPT !.absent = FALSE])
AddBack == AddBackA(Advance)

\* Every step but the change of limit, with Rf and Ab for the removal and the return: the removal's witness gives its
\* own removal through this.
NextFriend(P, Q, St, M(_), Ex, T, R, Se(_), Ac(_), Pl(_, _), Rf, Ab) ==
    \/ P
    \/ SignIn
    \/ \E c \in BOOLEAN : Reconfigure(c)
    \/ Q
    \/ St
    \/ \E m \in Messages(s) : M(m)
    \/ Broken
    \/ Ex
    \/ T
    \/ Shutdown
    \/ R
    \/ \E v \in Settings : Se(v)
    \/ \E a \in Accounts \cup {""} : Ac(a)
    \/ \E d \in Dirs \cup {""}, h \in BOOLEAN : Pl(d, h)
    \/ Rf
    \/ Ab
    \/ RestartUnknown
    \/ \E v \in BOOLEAN : Learn(v)

\* Every step but the change of limit, for the limit's witnesses to give their own.
NextCore(P, Q, St, M(_), Ex, T, R, Se(_), Ac(_), Pl(_, _)) ==
    NextFriend(P, Q, St, M, Ex, T, R, Se, Ac, Pl, RemoveFriend, AddBack)

NextWithPlace(P, Q, St, M(_), Ex, T, R, Se(_), Ac(_), Pl(_, _)) ==
    NextCore(P, Q, St, M, Ex, T, R, Se, Ac, Pl) \/ \E v \in BOOLEAN : SetLimit(v)

NextWithSet(P, Q, St, M(_), Ex, T, R, Se(_), Ac(_)) == NextWithPlace(P, Q, St, M, Ex, T, R, Se, Ac, SetPlace)

NextWithAll(P, Q, St, M(_), Ex, T, R) == NextWithSet(P, Q, St, M, Ex, T, R, SetSetting, SetAccount)

NextWith(P, M(_), Ex, T, R) == NextWithAll(P, QueuePrompt, Stop, M, Ex, T, R)

\* Every step with A in place of Advance: the witnesses of the take, the setting, the account and the place break it
\* through this.
NextAdvance(A(_)) ==
    NextFriend(PromptWith(A), QueuePromptWith(A), Stop, LAMBDA m : MessageA(m, Gather, A), ExitA(ExitedWith, A),
               TimeoutA(End, End, A), RestartA(Restarted, A), SetSetting,
               LAMBDA a : SetAccountWith(LAMBDA x, b : A([x EXCEPT !.account = b]), a),
               LAMBDA d, h : SetPlaceWith(LAMBDA x, e, g : A([x EXCEPT !.dir = e, !.here = g]), d, h),
               RemoveFriendA(Removed, A), AddBackA(A)) \/ (\E v \in BOOLEAN : SetLimit(v)) \/ BrokenA(A)

\* Every step with Rm in place of Removed: the removal's witness breaks it through this.
NextRemove(Rm(_)) ==
    NextFriend(Prompt, QueuePrompt, Stop, Message, Exit, Timeout, Restart, SetSetting, SetAccount, SetPlace,
               RemoveFriendWith(Rm), AddBack) \/ \E v \in BOOLEAN : SetLimit(v)

\* Every step with Tk in place of Take: the take's witnesses break it through this.
NextTake(Tk(_)) == NextAdvance(LAMBDA x : AdvanceWith(Tk, x))

Next == NextWith(Prompt, Message, Exit, Timeout, Restart)

\* The fairness the liveness needs: deadlines pass, KILL ends a driver, and a person whose friend's turn never ends
\* stops it (there is no deadline on a working turn, by design).
ExitKilled == s.phase = "ending" /\ s.stage = "killed" /\ Exit
StopWorking == Working(s) /\ Stop
Spec == Init /\ [][Next]_vars /\ WF_vars(Timeout) /\ WF_vars(StopWorking) /\ WF_vars(ExitKilled)
\* The spec without fairness, which a finite trace of the code (SupervisorTrace.tla) is checked against.
SafetySpec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* Properties

TypeOK ==
    /\ s.phase \in Phases
    /\ s.stage \in Stages
    /\ (s.phase = "ending") = (s.stage # "none")
    /\ s.next \in Nat \ {0}
    /\ s.open \in 0 .. (s.next - 1)
    /\ s.failed \in Nat
    /\ \A i \in 1 .. Len(s.fx) : s.fx[i].k \in Kinds
    /\ \A p \in DOMAIN s.fate : s.fate[p] \in Fates
    /\ sent \in 0 .. Prompts
    /\ s.setting \in Settings
    /\ s.told \in Settings
    /\ (s.procs = 0) => s.told = Default
    /\ s.account \in Accounts \cup {""}
    /\ s.owner \in Accounts \cup {""}
    /\ (s.procs = 0) => s.drv = "" /\ ~s.retiring
    /\ s.crashed \in BOOLEAN
    /\ s.unconf \in BOOLEAN
    /\ s.retiring => s.phase = "ending"
    /\ s.dir \in Dirs \cup {""}
    /\ s.lim \in BOOLEAN /\ s.dlim \in BOOLEAN
    /\ (s.procs = 0) => ~s.dlim
    /\ s.here \in BOOLEAN
    /\ s.summary \in BOOLEAN
    /\ (s.procs = 0) => s.ddir = ""
    /\ (s.session = "") => s.sdir = ""
    /\ s.absent \in BOOLEAN
    /\ s.rm \in 0 .. MaxRemovals
    /\ s.removing \in BOOLEAN
    /\ s.known \in BOOLEAN

\* At most one driver process alive.
OneDriver == s.procs <= 1 /\ (s.procs = 1) = (s.phase # "none")

\* At most one turn open, and only while a driver works on it; every turn written is the open one or ended.
OneTurn ==
    /\ (s.open # 0) = (s.phase \in {"working", "stopping"})
    /\ Cardinality({p \in DOMAIN s.fate : s.fate[p] = "open"}) <= 1
    /\ \A i \in 1 .. Len(s.written) :
        s.fate[s.written[i].p] = "open" => s.written[i].t = s.open /\ s.written[i].p = s.prompt

\* A turn number is never reused and never goes down, across driver and server restarts.
TurnsIncrease ==
    /\ \A i \in 1 .. Len(s.written) : s.written[i].t < s.next
    /\ \A i, j \in 1 .. Len(s.written) : i < j => s.written[i].t < s.written[j].t

\* Each prompt is written to a driver at most once, ever.
AtMostOnce == \A i, j \in 1 .. Len(s.written) : i # j => s.written[i].p # s.written[j].p

\* The queue: within the bound, each prompt in it once and queued, and every queued prompt in it.
QueueSound ==
    /\ Len(s.queue) <= MaxWaiting
    /\ \A i, j \in 1 .. Len(s.queue) : i # j => s.queue[i] # s.queue[j]
    /\ \A p \in DOMAIN s.fate : (s.fate[p] = "queued") = (\E i \in 1 .. Len(s.queue) : s.queue[i] = p)

\* A prompt waits only while the driver cannot take it, and no more than the bound wait.
WaitingSound ==
    /\ Len(s.waiting) <= MaxWaiting
    /\ s.phase = "idle" => s.waiting = <<>>
    /\ \A i \in 1 .. Len(s.waiting) : s.fate[s.waiting[i]] = "waiting"

\* The failed-start limit holds.
LimitHolds == s.failed <= MaxFailedStarts

\* A stop is written only for the open turn, and at most once for a turn.
StopOnce == \A i, j \in 1 .. Len(s.stops) : i # j => s.stops[i] # s.stops[j]
StopForOpen == [][Len(s'.stops) > Len(s.stops) => Len(s'.stops) = Len(s.stops) + 1 /\ s.open # 0 /\
                                                  s'.stops[Len(s'.stops)] = s.open]_vars

\* What the step took from the queue: the prompts in it before and in it no more.
TakenBy(x, y) == {x.queue[i] : i \in 1 .. Len(x.queue)} \ {y.queue[i] : i \in 1 .. Len(y.queue)}

\* A queued prompt leaves the queue only when no turn is open (or the step ended the open one), the oldest first,
\* one per turn: it alone waits, or it is the turn just opened, and it shows as the person's entry.
TakesAtTurnEnd ==
    [][LET taken == TakenBy(s, s') IN
       taken # {} =>
         /\ taken = {Head(s.queue)}
         /\ s.open = 0 \/ s'.fate[s.prompt] \in {"answered", "cut"}
         /\ s.waiting = <<>>
         /\ \/ s'.waiting = <<Head(s.queue)>> /\ s'.open = 0
            \/ s'.waiting = <<>> /\ s'.prompt = Head(s.queue)
         /\ \E i \in 1 .. Len(s'.fx) : s'.fx[i] = E("P", 0, Head(s.queue))]_vars

\* No effect for a turn that is not open: every entry of a turn is for the turn open before the step, but for the
\* notice of a turn a stop cancelled before it began.
EffectsForOpen ==
    [][\A i \in 1 .. Len(s'.fx) : s'.fx[i].k \in TurnKinds =>
         \/ s'.fx[i].t = s.open /\ s.open # 0
         \/ s'.fx[i] = E("K", 0, 0) /\ s'.asked /\ s.phase = "starting"]_vars

\* The prompts a step cancelled: waiting before, cut after (only a stop of a start does that).
CancelledBy(x, y) == {p \in DOMAIN x.fate : x.fate[p] = "waiting" /\ y.fate[p] = "cut"}

\* A stop ends what runs when it arrives: the open turn, by a stop written for it; the turn about to begin while the
\* driver starts, the oldest waiting prompt, cancelled. Finding the turn already stopping, or nothing running, it
\* changes nothing.
StopEndsWhatRuns ==
    [][s'.asked =>
         CASE s.phase = "working"
              -> s' = [s EXCEPT !.fx = <<>>, !.asked = TRUE, !.signin = FALSE, !.removing = FALSE, !.phase = "stopping",
                                !.stops = Append(@, s.open)]
           [] s.phase = "starting" /\ s.waiting # <<>>
              -> s' = [s EXCEPT !.fx = <<E("K", 0, 0)>>, !.asked = TRUE, !.signin = FALSE, !.removing = FALSE, !.waiting = Tail(@),
                                !.fate[Head(s.waiting)] = "cut"]
           [] OTHER
              -> s' = [s EXCEPT !.fx = <<>>, !.asked = TRUE, !.signin = FALSE, !.removing = FALSE]]_vars

\* No stop is kept for later: a stop is written, or a turn cancelled, only in the step of the person's stop that
\* found it running, or in the step that tightened the limit under it (SetLimit), or in the step that took the friend
\* out of friends.json, for the turn open then. So a stop that found nothing never touches a turn that begins after it.
NoLatchedStop ==
    [][/\ Len(s'.stops) > Len(s.stops) => s'.asked \/ (s'.lim /\ ~s.lim) \/ s'.removing
       /\ CancelledBy(s, s') # {} => s'.asked]_vars

\* A turn ends only by its done or by a cut with its notice.
EndsAnsweredOrCut ==
    [][s.open # 0 /\ s'.open # s.open =>
         \/ s'.fate[s.prompt] = "answered"
         \/ s'.fate[s.prompt] = "cut" /\ \E i \in 1 .. Len(s'.fx) : s'.fx[i] = E("X", s.open, 0)]_vars

\* Text gathered for a turn that ends (other than by the server's own crash) goes into the tab before the turn's
\* closing notice.
TextKept ==
    [][s.gathered /\ s.open # 0 /\ s'.open # s.open /\ restarts' = restarts =>
         \E i \in 1 .. Len(s'.fx) : /\ s'.fx[i] = E("F", s.open, 0)
                                    /\ \A j \in 1 .. (i - 1) : s'.fx[j].k \notin {"X", "K"}]_vars

\* No prompt is written to a driver that said it has no credential: it exits next.
NoSendWithoutCredential == [][Len(s'.written) > Len(s.written) => ~s.nocred]_vars

\* A driver is started only when not held back, and with --resume the last session id (a start with no driver
\* configured names no program, and resumes nothing).
StartsSound == [][s'.procs > s.procs => ~s'.blocked /\ s'.resumed = IF s'.unconf THEN "" ELSE s'.session]_vars

\* A send is written with the setting the session has when it is written: a change applies from the next turn.
WrittenWithSetting ==
    [][Len(s'.written) > Len(s.written) => \A i \in (Len(s.written) + 1) .. Len(s'.written) :
                                               s'.written[i].v = s'.setting]_vars

\* A change of the setting never changes a turn in progress: no line is written, no driver started, no turn opened or
\* ended, no entry made (an idle driver told a field it clears may be ended, quietly).
SettingLeavesTurn ==
    [][s'.setting # s.setting =>
         /\ s'.written = s.written /\ s'.open = s.open /\ s'.waiting = s.waiting /\ s'.fx = <<>>
         /\ s'.procs = s.procs /\ (s'.phase # s.phase => s'.phase = "ending" /\ s'.retiring)]_vars

\* A send is written only to a driver started under the session's account as it is then.
NoSendOnOldAccount == [][Len(s'.written) > Len(s.written) => s'.drv = s'.account]_vars

\* A change of the account never ends a turn: the driver is ended only once no turn is open.
AccountKeepsTurn == [][s'.account # s.account => s'.open = s.open /\ s'.written = s.written]_vars

\* A send is written only to a driver started in the session's directory, on this machine.
NoSendInOldPlace == [][Len(s'.written) > Len(s.written) => s'.ddir = s'.dir /\ s'.here]_vars

\* A change of the place never ends a turn: the driver is ended only once no turn is open.
PlaceKeepsTurn == [][s'.dir # s.dir \/ s'.here # s.here => s'.open = s.open /\ s'.written = s.written]_vars

\* No session keeps running with a weaker limit than its friend's: a driver started without the limit, while the
\* session is limited, runs no turn; it is starting (nothing runs yet), stopping or being ended. Tightening under an
\* open turn stops it in the same step.
NeverWeaker == (s.procs > 0 /\ s.lim /\ ~s.dlim) => s.phase \in {"starting", "stopping", "ending"}
\* A send is written only to a driver started under the session's limit or a stricter one.
NoSendWeaker == [][Len(s'.written) > Len(s.written) => (s'.lim => s'.dlim)]_vars
\* Loosening never ends a turn: the driver started limited finishes it, and is ended once no turn is open.
LooseningKeepsTurn == [][s.lim /\ ~s'.lim => s'.open = s.open /\ s'.written = s.written /\ s'.stops = s.stops]_vars

\* A driver resumes a harness session only in the directory it was made in.
ResumesItsDir == [][s'.procs > s.procs /\ s'.resumed # "" => s.sdir = s'.ddir]_vars

\* A resume ID is sent only to a driver running under the account that reported it.
ResumesItsAccount == [][s'.procs > s.procs /\ s'.resumed # "" => s.owner = s'.drv]_vars

\* A harness session dropped from a conversation with turns owes a summary, and the next send carries what is owed,
\* once.
SummaryOwed == [][s.session # "" /\ s'.session = "" /\ s.next > 1 /\ restarts' = restarts => s'.summary]_vars
SummaryCarried ==
    [][Len(s'.written) > Len(s.written) => s'.written[Len(s'.written)].sum = s.summary /\ ~s'.summary]_vars

----------------------------------------------------------------------------
\* What the friend is doing, as the window's state cursor shows it (Supervisor::activity; sync/sessions.h Activity):
\* "down" while starting is held back; else "busy" while a turn is open or about to begin (a prompt waits with the
\* instance here and the friend in friends.json, or a driver starts); else "down" when no driver is configured, or the
\* instance is on another machine, or the friend is out of friends.json, or when the last driver failed and none has
\* said hello since, unless the server is stopping; else "idle". The trace
\* binds the code's to this one at every step (SupervisorTrace.tla).
ActivityOf(x) ==
    IF x.blocked THEN "down"
    ELSE IF x.open # 0 \/ (x.waiting # <<>> /\ x.here /\ ~x.absent) \/ x.phase = "starting" THEN "busy"
    ELSE IF x.unconf \/ ~x.here \/ x.absent \/ (x.crashed /\ ~x.down) THEN "down"
    ELSE "idle"

\* Idle (steady dark blue) is "waiting for the person": nothing runs, nothing waits, and nothing queued will be taken
\* without them (or the server is stopping, and takes nothing).
IdleWaitsForPersonWith(A(_)) ==
    A(s) = "idle" => /\ s.open = 0
                     /\ s.waiting = <<>>
                     /\ s.phase \in {"none", "idle", "ending"}
                     /\ s.queue = <<>> \/ s.down
IdleWaitsForPerson == IdleWaitsForPersonWith(ActivityOf)

\* A friend with no driver configured shows down (steady red), never idle: a prompt to it reaches no driver, and
\* nothing queued is taken. Only work still running (a turn open, a prompt waiting, a driver starting) shows busy.
NoDriverShowsDownWith(A(_)) ==
    s.unconf /\ s.open = 0 /\ s.waiting = <<>> /\ s.phase # "starting" => A(s) = "down"
NoDriverShowsDown == NoDriverShowsDownWith(ActivityOf)

\* A driver that ends without being asked to (not ended for an account change, a setting, a missing credential or the
\* server stopping) never leaves the friend showing idle: it shows down, or busy when the work goes on (a prompt
\* waits, so another starts). Each step's half takes the activity as an argument, for the witnesses; the property
\* itself has its [] at the top, the only form TLC checks.
CrashShowsDownStep(A(_)) ==
    s'.procs < s.procs /\ s.phase # "ending" /\ restarts' = restarts => A(s') \in {"down", "busy"}
CrashShowsDown == [][CrashShowsDownStep(ActivityOf)]_vars

\* Down (red) never quietly turns into idle: it leaves only for busy (the person's prompt, a queued one, a start), when
\* a driver says hello, when the friend's driver is configured again, when its instance is on this machine again, when
\* its account signs in, when its friend is back in friends.json, or by a restart or the server stopping.
DownLeavesByWorkStep(A(_)) ==
    A(s) = "down" /\ A(s') = "idle" =>
        \/ s.phase = "starting" /\ s'.phase = "idle"
        \/ s'.signin
        \/ s.unconf /\ ~s'.unconf
        \/ ~s.here /\ s'.here
        \/ s.absent /\ ~s'.absent
        \/ restarts' > restarts
        \/ s'.down
DownLeavesByWork == [][DownLeavesByWorkStep(ActivityOf)]_vars

\* Liveness: every prompt accepted (sent or queued) is eventually answered or cut, or waits while starting drivers is
\* held back (until the person's next prompt), while the friend has no driver configured (until it has one again),
\* while the instance is on another machine (until it is here again), while the friend is out of friends.json (until it
\* is back), or while the server is stopping (the prompt is durable, for the next server).
SettlesFor(p) ==
    s.fate[p] \in {"queued", "waiting", "open"} ~>
        (s.fate[p] \in {"answered", "cut"} \/ s.blocked \/ s.unconf \/ ~s.here \/ s.absent \/ s.down)
Settles == \A p \in 1 .. Prompts : SettlesFor(p)
\* Settles in two halves, the odd prompts and the even: SettlesOdd /\ SettlesEven is Settles for any Prompts. TLC's
\* liveness check of the halves, each alone, takes well under half the time of the whole (MCSupervisorSwitch.cfg, two
\* prompts, on four cores of a loaded host: one prompt 38.7 s, both 94.2 s), so CI checks them in shards of their own
\* (tests/server/CMakeLists.txt, SPLIT).
SettlesOdd == \A p \in {q \in 1 .. Prompts : q % 2 = 1} : SettlesFor(p)
SettlesEven == \A p \in {q \in 1 .. Prompts : q % 2 = 0} : SettlesFor(p)

----------------------------------------------------------------------------
\* The friend out of friends.json.

\* While the friend is out nothing runs for it: no driver starts, idles or works; a turn still open is the one being
\* stopped, its stop written.
AbsentRunsNothing ==
    s.absent => /\ s.phase \in {"none", "stopping", "ending"}
                /\ s.open # 0 => s.stops # <<>> /\ s.stops[Len(s.stops)] = s.open

\* While the friend is out no driver starts and no prompt is written to one.
AbsentStartsNothing == [][s'.absent => s'.procs <= s.procs /\ Len(s'.written) = Len(s.written)]_vars

\* While the friend is out (and in the step that takes it out) nothing leaves the queue, no prompt becomes waiting (not
\* even one queued and taken in the same step), and no waiting prompt is cancelled: the person's prompts are kept for
\* its return.
AbsentKeepsPrompts ==
    [][s'.absent => /\ TakenBy(s, s') = {} /\ CancelledBy(s, s') = {}
                    /\ \A p \in DOMAIN s'.fate : s'.fate[p] = "waiting" => s.fate[p] = "waiting"]_vars

\* Once the friend is out, its driver is in time gone (its stopped turn ended, by its done or the stop's deadline), or
\* the friend is back.
AbsentEnds == s.absent ~> (~s.absent \/ s.phase = "none")

----------------------------------------------------------------------------
\* No friends list held.

\* A friend not known to be in the list never has a prompt written to its driver: none is written while the server
\* holds no list, nor in the step that applies one, unless that list has the friend.
NoSendUnknown == [][Len(s'.written) > Len(s.written) => s.known /\ s'.known /\ ~s'.absent]_vars

\* While no list is held the friend reads out, and nothing runs for it.
UnknownRunsNothing == ~s.known => s.absent /\ s.procs = 0 /\ s.open = 0

=============================================================================

----------------------------- MODULE Supervisor -----------------------------
(***************************************************************************)
(* The driver supervisor (src/server/supervisor.h): what nexus-server does *)
(* with one friend tab's driver process. It starts a driver when a prompt  *)
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
(* The supervisor's state is one record, s. Its fields next, open, waiting *)
(* and session are durable (the store): each step's effects and durable    *)
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
(* resumed (the --resume the last start was given, "" for none).           *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Prompts,          \* how many prompts the person sends, numbered 1..Prompts
    MaxWaiting,       \* how many prompts may wait (the hub's queue limit)
    MaxFailedStarts,  \* consecutive failed starts before the supervisor stops starting drivers
    MaxRestarts,      \* how many times the server restarts in a behaviour
    Sessions          \* the session ids a driver may report

ASSUME MaxWaiting >= 1 /\ MaxFailedStarts >= 1

VARIABLES
    s,          \* the supervisor, with its ghosts
    sent,       \* how many prompts the person has sent
    restarts    \* how many times the server restarted

vars == <<s, sent, restarts>>

Phases == {"none", "starting", "idle", "working", "stopping", "ending"}
Stages == {"none", "closed", "termed", "killed"}   \* while ending: input closed, then TERM sent, then KILL sent
Fates == {"unsent", "refused", "waiting", "open", "answered", "cut"}
Kinds == {"P", "F", "T", "E", "K", "X", "N"}
TurnKinds == {"F", "T", "E", "K", "X"}
Billings == {"subscription", "api-key", "none"}
Outcomes == {"ok", "stopped", "error"}
E(k, t, p) == [k |-> k, t |-> t, p |-> p]

Init ==
    /\ sent = 0
    /\ restarts = 0
    /\ s = [phase |-> "none", stage |-> "none", open |-> 0, prompt |-> 0, next |-> 1, waiting |-> <<>>,
            session |-> "", gathered |-> FALSE, finished |-> FALSE, failed |-> 0, blocked |-> FALSE,
            down |-> FALSE, procs |-> 0, written |-> <<>>, fate |-> [p \in 1 .. Prompts |-> "unsent"],
            fx |-> <<>>, resumed |-> ""]

----------------------------------------------------------------------------
\* The parts of a step, each from one supervisor record to the next.

Fx(x, e) == [x EXCEPT !.fx = Append(@, e)]

\* The text gathered for the open turn goes into the tab as the friend's entry.
Flush(x) == IF x.gathered THEN [Fx(x, E("F", x.open, 0)) EXCEPT !.gathered = FALSE] ELSE x

\* The open turn ends without its done: a notice that it was cut short. The prompt is never written again.
CutOpen(x) == [Fx(x, E("X", x.open, 0)) EXCEPT !.fate[x.prompt] = "cut", !.open = 0, !.prompt = 0]

\* The driver is gone or going: the text gathered, a notice of why, and the open turn cut.
Abandon(x) ==
    LET y == Fx(Flush(x), E("N", 0, 0)) IN
    IF y.open # 0 THEN CutOpen(y) ELSE y

\* End the driver: abandon it and close its input; TERM and KILL follow on deadlines.
End(x) == [Abandon(x) EXCEPT !.phase = "ending", !.stage = "closed"]

\* The open turn's done.
Close(x) == [x EXCEPT !.fate[x.prompt] = "answered", !.open = 0, !.prompt = 0, !.phase = "idle",
                      !.finished = TRUE, !.failed = 0]

\* Write the oldest waiting prompt to an idle driver as the next turn.
WriteNext(x) ==
    IF x.phase = "idle" /\ x.waiting # <<>>
    THEN LET p == Head(x.waiting) IN
         [x EXCEPT !.open = x.next, !.prompt = p, !.next = x.next + 1, !.waiting = Tail(x.waiting),
                   !.written = Append(@, [p |-> p, t |-> x.next]), !.fate[p] = "open", !.phase = "working"]
    ELSE x

\* Start a driver when a prompt waits and none is alive, with --resume the last session id.
StartDriver(x) ==
    IF x.phase = "none" /\ x.waiting # <<>> /\ ~x.blocked /\ ~x.down
    THEN [x EXCEPT !.phase = "starting", !.procs = @ + 1, !.finished = FALSE, !.resumed = x.session]
    ELSE x

\* What follows every step.
Advance(x) == WriteNext(StartDriver(x))

\* Every step starts with no effects.
Fresh == [s EXCEPT !.fx = <<>>]

\* The driver is gone: a failed start is one that finished no turn.
ExitedWith(x) ==
    LET y == IF x.phase = "ending" THEN x ELSE Abandon(x)
        count == ~y.finished /\ ~y.blocked /\ ~y.down
        f == IF count THEN y.failed + 1 ELSE y.failed
        z == [y EXCEPT !.phase = "none", !.stage = "none", !.procs = @ - 1, !.failed = f, !.finished = FALSE,
                       !.gathered = FALSE]
    IN IF count /\ f >= MaxFailedStarts THEN [Fx(z, E("N", 0, 0)) EXCEPT !.blocked = TRUE] ELSE z

\* A message from the driver.
ReceiveWith(x, m, Txt(_, _)) ==
    CASE x.phase = "ending"   -> x
      [] x.phase = "starting" -> IF m.kind = "hello" /\ m.v = "1" THEN [x EXCEPT !.phase = "idle"] ELSE End(x)
      [] m.kind = "hello"      -> End(x)
      [] m.kind = "credential" -> IF m.v = "subscription" THEN x ELSE [End(x) EXCEPT !.blocked = TRUE]
      [] m.kind = "session"    -> [x EXCEPT !.session = m.v]
      [] m.kind = "error"      -> IF m.t = 0 THEN Fx(x, E("N", 0, 0))
                                  ELSE IF m.t = x.open THEN Fx(x, E("E", m.t, 0)) ELSE x
      [] m.kind = "text"       -> Txt(x, m)
      [] m.t # x.open          -> End(x)
      [] m.kind = "text-end"   -> Flush(x)
      [] m.kind = "tool"       -> Fx(Flush(x), E("T", x.open, 0))
      [] m.kind = "done"       -> Close(IF m.v = "stopped" THEN Fx(Flush(x), E("K", x.open, 0)) ELSE Flush(x))
      [] OTHER                 -> x

\* text gathers for the open turn; for any other turn it breaks the protocol.
Gather(x, m) == IF m.t = x.open /\ x.open # 0 THEN [x EXCEPT !.gathered = TRUE] ELSE End(x)

\* The messages a driver may write here: every kind, for turns about the open one.
Turns(x) == {t \in {x.open - 1, x.open, x.open + 1, x.next - 1, x.next} : t >= 1}
Messages(x) ==
    {[kind |-> "hello", t |-> 0, v |-> v] : v \in {"1", "2"}} \cup
    {[kind |-> "credential", t |-> 0, v |-> b] : b \in Billings} \cup
    {[kind |-> "session", t |-> 0, v |-> i] : i \in Sessions} \cup
    {[kind |-> k, t |-> t, v |-> ""] : k \in {"text", "text-end", "tool"}, t \in Turns(x)} \cup
    {[kind |-> "done", t |-> t, v |-> o] : t \in Turns(x), o \in Outcomes} \cup
    {[kind |-> "error", t |-> t, v |-> ""] : t \in Turns(x) \cup {0}}

----------------------------------------------------------------------------
\* The steps. Each witness passes a broken part through the With forms.

Keep == UNCHANGED <<sent, restarts>>

\* The person sends prompt p: refused with a notice past the bound; otherwise it shows, waits, and goes out when it can.
\* A prompt also lifts the hold that failed starts put on starting drivers.
PromptWith(Adv(_)) ==
    /\ ~s.down
    /\ sent < Prompts
    /\ sent' = sent + 1
    /\ UNCHANGED restarts
    /\ LET p == sent + 1
           x == Fresh
       IN s' = IF Len(x.waiting) >= MaxWaiting
               THEN [Fx(x, E("N", 0, 0)) EXCEPT !.fate[p] = "refused"]
               ELSE Adv([Fx(x, E("P", 0, p)) EXCEPT !.waiting = Append(@, p), !.fate[p] = "waiting",
                                                     !.blocked = FALSE, !.failed = IF x.blocked THEN 0 ELSE @])
Prompt == PromptWith(Advance)

\* The person stops the friend's work: a stop for the open turn, if one is open and no stop is out.
Stop ==
    /\ Keep
    /\ s' = LET x == Fresh IN IF x.phase = "working" THEN [x EXCEPT !.phase = "stopping"] ELSE x

MessageWith(m, Txt(_, _)) ==
    /\ s.procs > 0 /\ s.phase # "none"
    /\ Keep
    /\ s' = Advance(ReceiveWith(Fresh, m, Txt))
Message(m) == MessageWith(m, Gather)

\* A line the server refuses, or the driver's output or input closing: the driver is ended.
Broken ==
    /\ s.procs > 0 /\ s.phase \notin {"none", "ending"}
    /\ Keep
    /\ s' = End(Fresh)

\* The driver exits, by itself or by a signal.
ExitWith(Ex(_)) ==
    /\ s.procs > 0 /\ s.phase # "none"
    /\ Keep
    /\ s' = Advance(Ex(Fresh))
Exit == ExitWith(ExitedWith)

\* A deadline passes: no hello, no done after a stop, or a grace period while ending.
TimeoutWith(Starting(_), Stopping(_)) ==
    /\ \/ s.phase \in {"starting", "stopping"}
       \/ s.phase = "ending" /\ s.stage # "killed"
    /\ Keep
    /\ s' = LET x == Fresh IN
            Advance(CASE x.phase = "starting"                       -> Starting(x)
                      [] x.phase = "stopping"                       -> Stopping(x)
                      [] x.phase = "ending" /\ x.stage = "closed"   -> [x EXCEPT !.stage = "termed"]
                      [] OTHER                                      -> [x EXCEPT !.stage = "killed"])
Timeout == TimeoutWith(End, End)

\* The server stops: every driver is ended, and none is started again. A driver with no turn open is ended quietly.
Shutdown ==
    /\ ~s.down
    /\ Keep
    /\ s' = LET x == [Fresh EXCEPT !.down = TRUE] IN
            CASE x.phase \in {"none", "ending"} -> x
              [] x.open # 0                     -> End(x)
              [] OTHER                          -> [x EXCEPT !.phase = "ending", !.stage = "closed"]

\* The server starts again from what is durable. A turn left open is cut; waiting prompts go to a new driver.
RestartWith(R(_)) ==
    /\ restarts < MaxRestarts
    /\ restarts' = restarts + 1
    /\ UNCHANGED sent
    /\ s' = Advance(R(Fresh))
Restarted(x) ==
    LET y == IF x.open # 0 THEN CutOpen(x) ELSE x IN
    [y EXCEPT !.phase = "none", !.stage = "none", !.gathered = FALSE, !.finished = FALSE, !.failed = 0,
              !.blocked = FALSE, !.down = FALSE, !.procs = 0]
Restart == RestartWith(Restarted)

NextWith(P, M(_), Ex, T, R) ==
    \/ P
    \/ Stop
    \/ \E m \in Messages(s) : M(m)
    \/ Broken
    \/ Ex
    \/ T
    \/ Shutdown
    \/ R

Next == NextWith(Prompt, Message, Exit, Timeout, Restart)

\* The fairness the liveness needs: deadlines pass, KILL ends a driver, and a person whose friend's turn never ends
\* stops it (there is no deadline on a working turn, by design).
ExitKilled == s.phase = "ending" /\ s.stage = "killed" /\ Exit
Spec == Init /\ [][Next]_vars /\ WF_vars(Timeout) /\ WF_vars(Stop) /\ WF_vars(ExitKilled)
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

\* A prompt waits only while the driver cannot take it, and no more than the bound wait.
WaitingSound ==
    /\ Len(s.waiting) <= MaxWaiting
    /\ s.phase = "idle" => s.waiting = <<>>
    /\ \A i \in 1 .. Len(s.waiting) : s.fate[s.waiting[i]] = "waiting"

\* The failed-start limit holds.
LimitHolds == s.failed <= MaxFailedStarts

\* No effect for a turn that is not open: every entry of a turn is for the turn open before the step.
EffectsForOpen ==
    [][\A i \in 1 .. Len(s'.fx) : s'.fx[i].k \in TurnKinds => s'.fx[i].t = s.open /\ s.open # 0]_vars

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

\* A driver is started only when not held back, and with --resume the last session id.
StartsSound == [][s'.procs > s.procs => ~s'.blocked /\ s'.resumed = s'.session]_vars

\* Liveness: every prompt accepted is eventually answered or cut, or waits while starting drivers is held back (until
\* the person's next prompt) or the server is stopping (the prompt is durable, for the next server).
Settles ==
    \A p \in 1 .. Prompts :
        s.fate[p] \in {"waiting", "open"} ~> (s.fate[p] \in {"answered", "cut"} \/ s.blocked \/ s.down)

=============================================================================

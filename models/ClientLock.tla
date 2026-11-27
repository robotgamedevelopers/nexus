----------------------------- MODULE ClientLock -----------------------------
(***************************************************************************)
(* The client lock: a window (macOS, Windows, iOS) shows nothing of nexus  *)
(* and takes no input until the device's owner has passed the system's     *)
(* own authentication (Touch ID or the login password through Local-       *)
(* Authentication; Windows Hello; Face ID). nexus::ClientLock              *)
(* (src/client_lock.h) is this machine; the platform's authenticator and   *)
(* the OS's events around it are its environment.                          *)
(*                                                                         *)
(*   state    "locked", "prompting" (the system's prompt is up for the     *)
(*            current attempt) or "unlocked"                               *)
(*   why      why it is locked, said on the lock screen: "launch", a       *)
(*            relock event, "idle", or the refusal of the last attempt;    *)
(*            "none" while unlocked                                        *)
(*   attempt  the number of the last prompt (each prompt is a new one)     *)
(*   pending  attempts whose answer has not come back yet: an answer comes *)
(*            for every attempt, the current one or an old one             *)
(*   idle     seconds since the last input, while unlocked                 *)
(*   granted  the attempt whose "granted" unlocked it; 0 while locked      *)
(*   last     history: the step just taken, with its argument (a reason or *)
(*            an answer), its attempt, the seconds it let pass, and what   *)
(*            the code answered (a frame: drew content; input: delivered)  *)
(*                                                                         *)
(* Inputs: prompt (the overlay hotkey, lock screen, or Try again), the              *)
(* authenticator's answer to an attempt, a relock event from the OS        *)
(* (sleep, the screen locked, the user session switched, the app sent to   *)
(* the background on a phone), time passing (tick), the person's input,    *)
(* and a frame.                                                            *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Timeout,      \* idle seconds after which an unlocked window locks
          MaxAttempt    \* the bound on attempts, for a finite state space

VARIABLES state, why, attempt, pending, idle, granted, last

vars == <<state, why, attempt, pending, idle, granted, last>>

States   == {"locked", "prompting", "unlocked"}
Events   == {"sleep", "screen", "switch", "background"}
Answers  == {"granted", "failed", "cancelled", "unavailable"}
Refusals == Answers \ {"granted"}
Whys     == {"launch", "idle", "none"} \cup Events \cup Refusals
Ops      == {"init", "prompt", "answer", "relock", "tick", "input", "frame"}

Rec(op, arg, n, d, flag) == [op |-> op, arg |-> arg, n |-> n, d |-> d, flag |-> flag]

TypeOK ==
    /\ state \in States
    /\ why \in Whys
    /\ attempt \in 0..MaxAttempt
    /\ pending \subseteq 1..MaxAttempt
    /\ idle \in 0..Timeout
    /\ granted \in 0..MaxAttempt
    /\ last \in [op : Ops, arg : Whys \cup Answers \cup {""}, n : 0..MaxAttempt, d : 0..Timeout, flag : BOOLEAN]

\* Launch: locked, nothing asked yet.
Init ==
    /\ state = "locked"
    /\ why = "launch"
    /\ attempt = 0
    /\ pending = {}
    /\ idle = 0
    /\ granted = 0
    /\ last = Rec("init", "", 0, 0, FALSE)

\* Locking, from any state: what a relock, a refusal and the idle timeout share.
LockFor(w) ==
    /\ state' = "locked"
    /\ why' = w
    /\ idle' = 0
    /\ granted' = 0

\* The hotkey or lock screen asks the system: a new attempt, only from locked. ClientLock::prompt.
Prompt ==
    /\ state = "locked"
    /\ attempt < MaxAttempt
    /\ state' = "prompting"
    /\ attempt' = attempt + 1
    /\ pending' = pending \cup {attempt + 1}
    /\ UNCHANGED <<why, idle, granted>>
    /\ last' = Rec("prompt", "", attempt + 1, 0, FALSE)

\* Asked while prompting or unlocked: nothing (prompt returns 0). A second hotkey while the system's prompt is up does
\* not start another.
PromptIgnored ==
    /\ state # "locked"
    /\ UNCHANGED <<state, why, attempt, pending, idle, granted>>
    /\ last' = Rec("prompt", "", 0, 0, FALSE)

\* The authenticator's answer `a` to attempt `n`. Only the current attempt's answer, while it is still asked, counts: an
\* answer to an attempt a relock overtook, or to an older one, changes nothing. ClientLock::answer.
Answer(n, a) ==
    /\ n \in pending
    /\ pending' = pending \ {n}
    /\ IF state = "prompting" /\ n = attempt
          THEN IF a = "granted"
                  THEN state' = "unlocked" /\ why' = "none" /\ idle' = 0 /\ granted' = n
                  ELSE LockFor(a)
          ELSE UNCHANGED <<state, why, idle, granted>>
    /\ UNCHANGED attempt
    /\ last' = Rec("answer", a, n, 0, FALSE)

\* The OS: sleep, the screen locked, the session switched, the app sent to the background. From any state, a prompt
\* still up included (its answer will be stale). ClientLock::relock.
Relock(r) ==
    /\ LockFor(r)
    /\ UNCHANGED <<attempt, pending>>
    /\ last' = Rec("relock", r, 0, 0, FALSE)

\* `d` seconds pass. An unlocked window idle for Timeout locks. ClientLock::tick.
Tick(d) ==
    /\ IF state = "unlocked"
          THEN IF idle + d >= Timeout
                  THEN LockFor("idle")
                  ELSE idle' = idle + d /\ UNCHANGED <<state, why, granted>>
          ELSE UNCHANGED <<state, why, idle, granted>>
    /\ UNCHANGED <<attempt, pending>>
    /\ last' = Rec("tick", "", 0, d, FALSE)

\* The person's input: delivered only while unlocked, where it starts the idle time again. ClientLock::input.
Input ==
    /\ idle' = IF state = "unlocked" THEN 0 ELSE idle
    /\ UNCHANGED <<state, why, attempt, pending, granted>>
    /\ last' = Rec("input", "", 0, 0, state = "unlocked")

\* A frame: it draws nexus's content only while unlocked; otherwise the lock screen alone. ClientLock::drawsContent.
Frame ==
    /\ UNCHANGED <<state, why, attempt, pending, idle, granted>>
    /\ last' = Rec("frame", "", 0, 0, state = "unlocked")

\* A witness passes its broken action in place of the real one.
NextWith(P, A(_, _), R(_), T(_), I, F) ==
    \/ P
    \/ PromptIgnored
    \/ \E n \in pending, a \in Answers : A(n, a)
    \/ \E r \in Events : R(r)
    \/ \E d \in 1..Timeout : T(d)
    \/ I
    \/ F

Next == NextWith(Prompt, Answer, Relock, Tick, Input, Frame)

\* The system answers every prompt it was given (it may take its time, and the answer may come late).
AnswerSome == \E n \in pending, a \in Answers : Answer(n, a)

Spec == Init /\ [][Next]_vars /\ WF_vars(AnswerSome)

\* Without the fairness: what a finite code trace is checked against.
SafeSpec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* Properties

\* Unlocked only by the system's "granted" for the attempt that is current.
UnlockedByAuth == state = "unlocked" => granted > 0 /\ granted = attempt

\* Unlocked exactly when there is no reason to show.
UnlockedSaysNothing == (state = "unlocked") <=> (why = "none")

\* The idle time is counted only while unlocked, and never reaches the timeout there.
IdleOnlyUnlocked == state # "unlocked" => idle = 0
IdleBelowTimeout == idle < Timeout

\* While prompting, the current attempt's answer is still to come.
PromptPending == state = "prompting" => attempt \in pending

\* A frame draws content if and only if unlocked: never while locked or prompting.
ContentOnlyUnlocked == [][last'.op = "frame" => last'.flag = (state = "unlocked")]_vars

\* Input reaches nexus if and only if unlocked.
InputOnlyUnlocked == [][last'.op = "input" => last'.flag = (state = "unlocked")]_vars

\* The only way into unlocked: "granted" for the current attempt, while it is asked.
UnlockOnlyByCurrent ==
    [][state # "unlocked" /\ state' = "unlocked" =>
         /\ state = "prompting"
         /\ last'.op = "answer"
         /\ last'.arg = "granted"
         /\ last'.n = attempt]_vars

\* An answer that is not the current attempt's changes nothing.
StaleIgnored == [][last'.op = "answer" /\ ~(state = "prompting" /\ last'.n = attempt) => state' = state]_vars

\* A refusal of the current attempt leaves it locked and says which refusal.
RefusalSaysWhy ==
    [][last'.op = "answer" /\ last'.arg # "granted" /\ state = "prompting" /\ last'.n = attempt =>
         state' = "locked" /\ why' = last'.arg]_vars

\* Every relock event locks, whatever the state, a prompt that is up included, and says why.
RelockLocks == [][last'.op = "relock" => state' = "locked" /\ why' = last'.arg]_vars

\* An unlocked window whose idle time reaches the timeout locks.
IdleLocks == [][last'.op = "tick" /\ state = "unlocked" /\ idle + last'.d >= Timeout =>
                  state' = "locked" /\ why' = "idle"]_vars

\* A prompt starts only from locked: never a second while one is up.
PromptOnlyLocked == [][state # "prompting" /\ state' = "prompting" => state = "locked"]_vars

\* A prompt ends: the system's answer comes, or a relock overtakes it.
PromptEnds == (state = "prompting") ~> (state # "prompting")

=============================================================================

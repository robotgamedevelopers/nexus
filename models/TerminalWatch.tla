--------------------------- MODULE TerminalWatch ---------------------------
(***************************************************************************)
(* Synchronization of one terminal's foreground process group between the  *)
(* terminals' background poll thread and the frame thread:                 *)
(* src/terminal_watch.h, used by nexus::PraxTerminals in src/terminals.cpp.*)
(*                                                                         *)
(* Both threads read the OS foreground process group outside the mutex:    *)
(*   - the background thread every 250 ms (or at once when `recheck` asks),*)
(*     so a session that starts or ends with no new title is noticed;      *)
(*   - the frame thread when the terminal's title changes, so TabLabel     *)
(*     sees the foreground as of that title without waiting for the poll.  *)
(*                                                                         *)
(* Under the mutex, `shared` holds the latest committed observation        *)
(* [seq, group, remote], and `recheck` records that a commit overlapped an *)
(* unlocked read on the other thread so the background thread polls again. *)
(* On the frame thread, `seen` holds what TabLabel last received.          *)
(***************************************************************************)
EXTENDS Integers, TLC

CONSTANTS
    Groups,         \* process groups the terminal may place in the foreground
    SshGroups,      \* those among Groups whose argv names an ssh destination
    MaxChanges,     \* bound on foreground changes in an instance
    MaxTitles       \* bound on title-change observations in an instance

NoGroup == -1
NoRemote == 0

ASSUME NoGroup \notin Groups /\ NoRemote \notin SshGroups /\ SshGroups \subseteq Groups /\ Groups # {}

RemoteOf(g) == IF g \in SshGroups THEN g ELSE NoRemote

VARIABLES
    osGroup,        \* the terminal's foreground process group in the OS now
    shared,         \* ForegroundSeen under mutex_: [seq, group, remote]
    seen,           \* ForegroundSeen on the frame thread: [seq, group, remote]
    recheck,        \* true when an overlapped commit needs another background poll
    poll,           \* background poll in flight outside mutex_
    frame,          \* frame-thread title observation in flight outside mutex_
    changes,        \* foreground changes taken so far
    titles,         \* title observations started so far
    last            \* history: the step just taken and what it answered

vars == <<osGroup, shared, seen, recheck, poll, frame, changes, titles, last>>
ModelView == <<osGroup, shared, seen, recheck, poll, frame, changes, titles>>

EmptySeen == [seq |-> 0, group |-> NoGroup, remote |-> NoRemote]
IdlePoll == [active |-> FALSE, snap |-> EmptySeen, group |-> NoGroup, remote |-> NoRemote]
IdleFrame == [active |-> FALSE, group |-> NoGroup, remote |-> NoRemote]

MaxSeq == 1 + MaxChanges + 2 * MaxTitles + 2

SeenType == [seq : 0..MaxSeq, group : Groups \cup {NoGroup}, remote : SshGroups \cup {NoRemote}]

Ops == {"init", "env", "frame-take", "frame-start", "frame-commit", "poll-start", "poll-commit"}

TypeOK ==
    /\ osGroup \in Groups
    /\ shared \in SeenType
    /\ seen \in SeenType
    /\ recheck \in BOOLEAN
    /\ poll \in [active : BOOLEAN, snap : SeenType, group : Groups \cup {NoGroup}, remote : SshGroups \cup {NoRemote}]
    /\ frame \in [active : BOOLEAN, group : Groups \cup {NoGroup}, remote : SshGroups \cup {NoRemote}]
    /\ changes \in 0..MaxChanges
    /\ titles \in 0..MaxTitles
    /\ last \in [op : Ops, changed : BOOLEAN, recheck : BOOLEAN]

Init ==
    /\ osGroup \in Groups
    /\ shared = EmptySeen
    /\ seen = EmptySeen
    /\ recheck = FALSE
    /\ poll = IdlePoll
    /\ frame = IdleFrame
    /\ changes = 0
    /\ titles = 0
    /\ last = [op |-> "init", changed |-> FALSE, recheck |-> FALSE]

\* The OS switches the terminal's foreground process group (fork/exec into ssh, or the shell reclaiming the tty).
EnvChange ==
    /\ changes < MaxChanges
    /\ \E g \in Groups \ {osGroup} : osGroup' = g
    /\ changes' = changes + 1
    /\ UNCHANGED <<shared, seen, recheck, poll, frame, titles>>
    /\ last' = [op |-> "env", changed |-> FALSE, recheck |-> FALSE]

\* takeForegroundWatch(shared, seen) under mutex_ at the start of PraxTerminals::observe.
FrameTake ==
    /\ ~frame.active
    /\ shared.seq > seen.seq
    /\ seen' = shared
    /\ UNCHANGED <<osGroup, shared, recheck, poll, frame, changes, titles>>
    /\ last' = [op |-> "frame-take", changed |-> FALSE, recheck |-> FALSE]

\* PraxTerminals::observe sees a new title: after syncing `seen` from `shared`, it reads foregroundGroup outside mutex_,
\* and processArguments only when foregroundNeedsArguments(seen, g) holds.
ForegroundNeedsArgs(s, g) == s.seq = 0 \/ g # s.group

FrameStartWith(NeedsArgs(_, _)) ==
    /\ ~frame.active
    /\ titles < MaxTitles
    /\ LET synced == IF shared.seq > seen.seq THEN shared ELSE seen
           g == osGroup
           r == IF NeedsArgs(synced, g) THEN RemoteOf(g) ELSE synced.remote
       IN /\ seen' = synced
          /\ frame' = [active |-> TRUE, group |-> g, remote |-> r]
          /\ titles' = titles + 1
          /\ UNCHANGED <<osGroup, shared, recheck, poll, changes>>
          /\ last' = [op |-> "frame-start", changed |-> FALSE, recheck |-> FALSE]

FrameStart == FrameStartWith(ForegroundNeedsArgs)

\* commitFrameForeground(shared, seen, group, remote) under mutex_.
FrameCommit ==
    /\ frame.active
    /\ LET overlapped == shared.seq # seen.seq
           next == (IF shared.seq > seen.seq THEN shared.seq ELSE seen.seq) + 1
           updated == [seq |-> next, group |-> frame.group, remote |-> frame.remote]
       IN /\ seen' = updated
          /\ shared' = updated
          /\ recheck' = (recheck \/ overlapped)
          /\ frame' = IdleFrame
          /\ UNCHANGED <<osGroup, poll, changes, titles>>
          /\ last' = [op |-> "frame-commit", changed |-> TRUE, recheck |-> overlapped]

\* Background poll thread snapshots `shared` under mutex_ (clearing `recheck`), then reads foregroundGroup outside it.
PollNeedsArgs(snap, g) == snap.seq = 0 \/ g # snap.group

PollStart ==
    /\ ~poll.active
    /\ LET snap == shared
           g == osGroup
           r == IF PollNeedsArgs(snap, g) THEN RemoteOf(g) ELSE snap.remote
       IN /\ recheck' = FALSE
          /\ poll' = [active |-> TRUE, snap |-> snap, group |-> g, remote |-> r]
          /\ UNCHANGED <<osGroup, shared, seen, frame, changes, titles>>
          /\ last' = [op |-> "poll-start", changed |-> FALSE, recheck |-> FALSE]

\* commitPollForeground(shared, snapshot, group, remote) under mutex_.
PollCommit ==
    /\ poll.active
    /\ IF shared.seq # poll.snap.seq
       THEN /\ recheck' = TRUE
            /\ poll' = IdlePoll
            /\ UNCHANGED <<osGroup, shared, seen, frame, changes, titles>>
            /\ last' = [op |-> "poll-commit", changed |-> FALSE, recheck |-> TRUE]
       ELSE IF ~PollNeedsArgs(poll.snap, poll.group)
            THEN /\ poll' = IdlePoll
                 /\ UNCHANGED <<osGroup, shared, seen, recheck, frame, changes, titles>>
                 /\ last' = [op |-> "poll-commit", changed |-> FALSE, recheck |-> FALSE]
            ELSE /\ shared' = [seq |-> poll.snap.seq + 1, group |-> poll.group, remote |-> poll.remote]
                 /\ poll' = IdlePoll
                 /\ UNCHANGED <<osGroup, seen, recheck, frame, changes, titles>>
                 /\ last' = [op |-> "poll-commit", changed |-> TRUE, recheck |-> FALSE]

NextWith(FS, FC, PC) == EnvChange \/ FrameTake \/ FS \/ FC \/ PollStart \/ PC

Next == NextWith(FrameStart, FrameCommit, PollCommit)

\* Fairness: an in-flight frame commit finishes, the 250 ms poll loop keeps running, and a frame takes a newer shared
\* observation (prAppWake wakes the window whenever PollCommit sets changed).
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(FrameCommit)
    /\ WF_vars(PollStart)
    /\ WF_vars(PollCommit)
    /\ WF_vars(FrameTake)

SafeSpec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* Properties

\* `seen` never runs ahead of `shared`: every committed observation enters `shared`.
SeqOrdered == seen.seq <= shared.seq

\* Every committed observation carries a real group and that group's own remote label.
RemoteMatchesGroup ==
    /\ (shared.seq > 0 => (shared.group \in Groups /\ shared.remote = RemoteOf(shared.group)))
    /\ (seen.seq > 0 => (seen.group \in Groups /\ seen.remote = RemoteOf(seen.group)))

\* Sequence numbers never go backward.
SeqMonotone == [][shared'.seq >= shared.seq /\ seen'.seq >= seen.seq]_vars

\* A title-change commit immediately gives `seen` the foreground group and remote read for that title.
FrameSeesTitleGroup ==
    [][last'.op = "frame-commit" =>
        (seen'.group = frame.group /\ seen'.remote = RemoteOf(frame.group))]_vars

\* A background poll whose snapshot was overtaken by a frame commit never overwrites `shared` and asks for a recheck.
PollNeverClobbersFrame ==
    [][(last'.op = "poll-commit" /\ shared.seq # poll.snap.seq) =>
        (shared' = shared /\ recheck' /\ ~last'.changed /\ last'.recheck)]_vars

\* A frame commit that overtakes a background commit between FrameStart and FrameCommit asks for a recheck.
OverlapAsksRecheck ==
    [][(last'.op = "frame-commit" /\ shared.seq # seen.seq) =>
        (recheck' /\ last'.recheck)]_vars

\* Once foreground and title changes stop, `seen` settles to the terminal's true foreground group and remote label,
\* even across G_sh -> G_ssh -> G_sh ABA transitions within one poll window and with no further title changes.
SettlesToForeground ==
    <>[] (seen.group = osGroup /\ seen.remote = RemoteOf(osGroup) /\ shared.seq > 0 /\ ~recheck)

=============================================================================

-------------------------- MODULE WWatchStalePoll --------------------------
\* Broken TerminalWatch: PollCommit overwrites `shared` even when `shared.seq` moved while the poll was unlocked.
\* Caught by PollNeverClobbersFrame.
EXTENDS TerminalWatch

BrokenPollCommit ==
    /\ poll.active
    /\ IF ~PollNeedsArgs(poll.snap, poll.group)
       THEN /\ poll' = IdlePoll
            /\ UNCHANGED <<osGroup, shared, seen, recheck, frame, changes, titles>>
            /\ last' = [op |-> "poll-commit", changed |-> FALSE, recheck |-> FALSE]
       ELSE /\ shared' = [seq |-> shared.seq + 1, group |-> poll.group, remote |-> poll.remote]
            /\ poll' = IdlePoll
            /\ UNCHANGED <<osGroup, seen, recheck, frame, changes, titles>>
            /\ last' = [op |-> "poll-commit", changed |-> TRUE, recheck |-> FALSE]

BrokenSpec == Init /\ [][NextWith(FrameStart, FrameCommit, BrokenPollCommit)]_vars

=============================================================================

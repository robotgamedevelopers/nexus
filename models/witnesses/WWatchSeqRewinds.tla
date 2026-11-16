------------------------- MODULE WWatchSeqRewinds -------------------------
\* Broken TerminalWatch: PollCommit resets `shared.seq` to 0 instead of advancing past `snapshot.seq`.
\* Caught by SeqMonotone.
EXTENDS TerminalWatch

BrokenPollCommit ==
    /\ poll.active
    /\ IF ~PollNeedsArgs(poll.snap, poll.group)
       THEN /\ poll' = IdlePoll
            /\ UNCHANGED <<osGroup, shared, seen, recheck, frame, changes, titles>>
            /\ last' = [op |-> "poll-commit", changed |-> FALSE, recheck |-> FALSE]
       ELSE /\ shared' = [seq |-> 0, group |-> poll.group, remote |-> poll.remote]
            /\ poll' = IdlePoll
            /\ UNCHANGED <<osGroup, seen, recheck, frame, changes, titles>>
            /\ last' = [op |-> "poll-commit", changed |-> TRUE, recheck |-> FALSE]

BrokenSpec == Init /\ [][NextWith(FrameStart, FrameCommit, BrokenPollCommit)]_vars

=============================================================================

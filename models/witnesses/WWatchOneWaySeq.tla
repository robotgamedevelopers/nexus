-------------------------- MODULE WWatchOneWaySeq --------------------------
\* Broken TerminalWatch: FrameCommit advances `seen.seq` without advancing `shared.seq`. Caught by SeqOrdered.
EXTENDS TerminalWatch

BrokenFrameCommit ==
    /\ frame.active
    /\ seen' = [seq |-> seen.seq + 1, group |-> frame.group, remote |-> frame.remote]
    /\ frame' = IdleFrame
    /\ UNCHANGED <<osGroup, shared, recheck, poll, changes, titles>>
    /\ last' = [op |-> "frame-commit", changed |-> TRUE, recheck |-> FALSE]

BrokenSpec == Init /\ [][NextWith(FrameStart, BrokenFrameCommit, PollCommit)]_vars

=============================================================================

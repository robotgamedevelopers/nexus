----------------------------- MODULE WWatchAba -----------------------------
\* Broken TerminalWatch (the pre-fix ABA bug): a frame's title observation updates `seen` alone without updating
\* `shared`, so after G_sh -> G_ssh -> G_sh within one poll window the poll sees G_sh = shared.group and never wakes.
EXTENDS TerminalWatch

BrokenFrameCommit ==
    /\ frame.active
    /\ seen' = [seq |-> seen.seq + 1, group |-> frame.group, remote |-> frame.remote]
    /\ frame' = IdleFrame
    /\ UNCHANGED <<osGroup, shared, recheck, poll, changes, titles>>
    /\ last' = [op |-> "frame-commit", changed |-> TRUE, recheck |-> FALSE]

BrokenNext == NextWith(FrameStart, BrokenFrameCommit, PollCommit)

BrokenSpec ==
    /\ Init
    /\ [][BrokenNext]_vars
    /\ WF_vars(BrokenFrameCommit)
    /\ WF_vars(PollStart)
    /\ WF_vars(PollCommit)
    /\ WF_vars(FrameTake)

=============================================================================

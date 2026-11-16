----------------------- MODULE WWatchFrameDropsTitle -----------------------
\* Broken TerminalWatch: FrameCommit leaves `seen` and `shared` unchanged instead of recording the title's group.
\* Caught by FrameSeesTitleGroup.
EXTENDS TerminalWatch

BrokenFrameCommit ==
    /\ frame.active
    /\ frame' = IdleFrame
    /\ UNCHANGED <<osGroup, shared, seen, recheck, poll, changes, titles>>
    /\ last' = [op |-> "frame-commit", changed |-> FALSE, recheck |-> FALSE]

BrokenSpec == Init /\ [][NextWith(FrameStart, BrokenFrameCommit, PollCommit)]_vars

=============================================================================

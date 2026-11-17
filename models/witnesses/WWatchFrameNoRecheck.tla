----------------------- MODULE WWatchFrameNoRecheck -----------------------
\* Broken TerminalWatch: FrameCommit never sets `recheck` when `shared.seq # seen.seq`. Caught by OverlapAsksRecheck.
EXTENDS TerminalWatch

BrokenFrameCommit ==
    /\ frame.active
    /\ LET next == (IF shared.seq > seen.seq THEN shared.seq ELSE seen.seq) + 1
           updated == [seq |-> next, group |-> frame.group, remote |-> frame.remote]
       IN /\ seen' = updated
          /\ shared' = updated
          /\ frame' = IdleFrame
          /\ UNCHANGED <<osGroup, recheck, poll, changes, titles>>
          /\ last' = [op |-> "frame-commit", changed |-> TRUE, recheck |-> FALSE]

BrokenSpec == Init /\ [][NextWith(FrameStart, BrokenFrameCommit, PollCommit)]_vars

=============================================================================

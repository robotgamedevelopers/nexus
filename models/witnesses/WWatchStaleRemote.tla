------------------------- MODULE WWatchStaleRemote -------------------------
\* Broken TerminalWatch: FrameStart reads process arguments only on the first observation (s.seq = 0), keeping the
\* previous group's remote label when the group changes. Caught by RemoteMatchesGroup.
EXTENDS TerminalWatch

BrokenNeedsArgs(s, g) == s.seq = 0

BrokenSpec == Init /\ [][NextWith(FrameStartWith(BrokenNeedsArgs), FrameCommit, PollCommit)]_vars

=============================================================================

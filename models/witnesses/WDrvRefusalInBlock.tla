------------------------- MODULE WDrvRefusalInBlock -------------------------
\* Reversed witness (found by a cold reader in the driver): a refusal while a text block is open writes the error and
\* the turn's done without ending the block first. NoDoneInBlock must reject it.
EXTENDS ClaudeDriver

BrokenRefuse == RefuseWith(<<>>)
BrokenResult(k) == ResultWith(k, BrokenRefuse, NotFound)

BrokenSpec == Init /\ [][NextWith(Stop, Apply, BrokenResult, BrokenRefuse, NotFound)]_vars
=============================================================================

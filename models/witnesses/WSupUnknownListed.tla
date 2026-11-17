--------------------------- MODULE WSupUnknownListed ---------------------------
\* Reversed witness: a server that starts holding no friends list reads every friend as in it, as Drivers::listed did
\* before (true with no registry): a friend taken out of friends.json, with a prompt queued, has it sent after a restart
\* whose committed files do not read. NoSendUnknown must reject it.
EXTENDS MCSupervisor

BrokenSpec == Init /\ [][Next \/ RestartUnknownWith(FALSE)]_vars
=============================================================================

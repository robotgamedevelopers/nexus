--------------------------- MODULE WSupUnknownListed ---------------------------
\* Reversed witness: a server that starts holding no AIs list reads every AI as in it, as Drivers::listed did
\* before (true with no registry): an AI taken out of ais.json, with a prompt queued, has it sent after a restart
\* whose committed files do not read. NoSendUnknown must reject it.
EXTENDS MCSupervisor

BrokenSpec == Init /\ [][Next \/ RestartUnknownWith(FALSE)]_vars
=============================================================================

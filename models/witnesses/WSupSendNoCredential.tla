------------------------- MODULE WSupSendNoCredential -------------------------
\* Reversed witness: a driver that reported no credential is left idle after its turn's done instead of being ended,
\* so the next waiting prompt is written to a driver about to exit. NoSendWithoutCredential must reject it.
EXTENDS MCSupervisor

ForgetNoCredential(m) ==
    /\ s.procs > 0 /\ s.phase # "none"
    /\ Keep
    /\ s' = Advance(ReceiveWith(IF m.kind = "done" THEN [Fresh EXCEPT !.nocred = FALSE] ELSE Fresh, m, Gather))

BrokenSpec == Init /\ [][NextWith(Prompt, ForgetNoCredential, Exit, Timeout, Restart)]_vars
=============================================================================

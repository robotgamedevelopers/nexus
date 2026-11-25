----------------------------- MODULE WSupStaleText -----------------------------
\* Reversed witness: text for a turn other than the open one is shown as the AI's reply instead of ending the
\* driver. EffectsForOpen must reject it.
EXTENDS MCSupervisor

BrokenGather(x, m) == IF m.t = x.open THEN [x EXCEPT !.gathered = TRUE] ELSE Fx(x, E("F", m.t, 0))

BrokenMessage(m) == MessageWith(m, BrokenGather)

BrokenSpec == Init /\ [][NextWith(Prompt, BrokenMessage, Exit, Timeout, Restart)]_vars
=============================================================================

--------------------------- MODULE WSupLimitNoStop ---------------------------
\* Reversed witness: tightening the limit under an open turn on a driver started without it does not stop that turn,
\* which runs on unlimited for as long as it takes. NeverWeaker must reject it.
EXTENDS MCSupervisor

NoStop(x, v) == Advance([x EXCEPT !.lim = v])

BrokenSpec == Init /\ [][NextCore(Prompt, QueuePrompt, Stop, Message, Exit, Timeout, Restart, SetSetting, SetAccount,
                                  SetPlace) \/ \E v \in BOOLEAN : SetLimitWith(NoStop, v)]_vars
=============================================================================

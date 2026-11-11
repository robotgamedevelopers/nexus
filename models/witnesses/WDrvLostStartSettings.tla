----------------------- MODULE WDrvLostStartSettings -----------------------
\* Reversed witness: when Claude does not know the --resume id once a turn runs, the new session starts with the
\* driver's start arguments, not the settings in use, so the turn goes on under other settings. SettingsNeverChangeOpen
\* must reject it.
EXTENDS ClaudeDriver

StartSettings == [f \in Fields |-> Start]
BrokenNotFound == NotFoundStopping(StartSettings) \/ NotFoundGoingOn(StartSettings)
BrokenResult(k) == ResultWith(k, Refuse, BrokenNotFound)

BrokenSpec == Init /\ [][NextWith(Stop, Apply, BrokenResult, Refuse, BrokenNotFound)]_vars
=============================================================================

----------------------- MODULE WDrvAskWithoutSecrets -----------------------
\* Reversed witness: nexus_ask_secret writes ask-secret without checking that a server line before the first send named
\* "secrets" (docs/secrets.md, "older server, newer driver"). AskGate must reject it.
EXTENDS ClaudeDriver

BrokenAsk ==
    /\ Running
    /\ ~stopping
    /\ asking = 0
    /\ asks < MaxAsk
    /\ asks' = asks + 1
    /\ asking' = asks + 1
    /\ wrote' = <<W("ask-secret", asks + 1)>>
    /\ UNCHANGED <<phase, resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
                   secrets, ghost>>

BrokenSpec == Init /\ [][NextSecretsWith(Stop, Apply, Result, Refuse, NotFound, BrokenAsk, Secret)]_vars
=============================================================================

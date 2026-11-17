-------------------------- MODULE WDrvTwoAsksOpen --------------------------
\* Reversed witness: a second nexus_ask_secret call while an ask is already open writes a second ask-secret instead of
\* answering refused: busy locally. AskGate must reject it.
EXTENDS ClaudeDriver

BrokenAsk ==
    /\ Running
    /\ ~stopping
    /\ secrets
    /\ asks < MaxAsk
    /\ asks' = asks + 1
    /\ asking' = asks + 1
    /\ wrote' = <<W("ask-secret", asks + 1)>>
    /\ UNCHANGED <<phase, resume, open, stage, stopping, textOpen, failure, intFor, pending, carry, inUse, last,
                   secrets, ghost>>

BrokenSpec == Init /\ [][NextSecretsWith(Stop, Apply, Result, Refuse, NotFound, BrokenAsk, Secret)]_vars
=============================================================================

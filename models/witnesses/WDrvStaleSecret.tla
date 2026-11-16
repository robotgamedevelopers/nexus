-------------------------- MODULE WDrvStaleSecret --------------------------
\* Reversed witness: a secret answer settles whatever ask is open without checking the ask number, so an earlier
\* stopped turn's late answer settles a later turn's ask. SecretMatchesAsk must reject it.
EXTENDS ClaudeDriver

BrokenSecret(a) == SecretWith(a, asking # 0)

BrokenSpec == Init /\ [][NextSecretsWith(Stop, Apply, Result, Refuse, NotFound, Ask, BrokenSecret)]_vars
=============================================================================

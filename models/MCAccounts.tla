------------------------------ MODULE MCAccounts ------------------------------
\* Accounts.tla on small instances (MCAccounts*.cfg), and the base of the reversed witnesses in
\* witnesses/WAcc*.tla. A configuration file cannot write a function: these are the providers of the accounts.
EXTENDS Accounts, TLC
\* AI names are interchangeable. All accounts properties quantify over them.
AIPermutations == Permutations(AIs)
\* Two Claude accounts and one Codex account.
ProvidersMixed == [a \in {"a1", "a2", "a3"} |-> IF a = "a3" THEN "codex" ELSE "claude"]
\* One of each.
ProvidersTwo == [a \in {"a1", "a3"} |-> IF a = "a3" THEN "codex" ELSE "claude"]
\* Two Claude accounts (MCAccountsLimit.cfg: one may be billed at API rate).
ProvidersPair == [a \in {"a1", "a2"} |-> "claude"]
\* One Claude account.
ProvidersOne == [a \in {"a1"} |-> "claude"]
=============================================================================

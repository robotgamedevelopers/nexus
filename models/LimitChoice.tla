---------------------------- MODULE LimitChoice ----------------------------
\* A concrete Fable offer for one interrupted request. The links and sign-ins
\* stay fixed here; if the account or request differs the offer is stale.
EXTENDS Naturals, Sequences
VARIABLES account, model, pending, work, commits, approved
vars == <<account, model, pending, work, commits, approved>>
Request == "unfinished"
Init == /\ account = "first" /\ model = "fable" /\ pending = TRUE
        /\ work = Request /\ commits = 0 /\ approved = "none"

Resolve(choice, fresh, requireApproval) ==
    IF (pending /\ fresh) \/ ~requireApproval
    THEN /\ account' = IF choice = "account" THEN "second" ELSE account
         /\ model' = IF choice = "model" THEN "opus" ELSE model
         /\ pending' = FALSE /\ commits' = commits + 1 /\ work' = work
         /\ approved' = IF pending /\ fresh THEN choice ELSE approved
    ELSE UNCHANGED vars

Attempt(requireApproval) ==
    \E choice \in {"account", "model"}, fresh \in BOOLEAN : Resolve(choice, fresh, requireApproval)

Spec == Init /\ [][Attempt(TRUE)]_vars
Broken == Init /\ [][Attempt(FALSE)]_vars
ChosenOnce == /\ commits = IF pending THEN 0 ELSE 1
              /\ approved = CASE pending -> "none"
                              [] account = "second" /\ model = "fable" -> "account"
                              [] account = "first" /\ model = "opus" -> "model"
                              [] OTHER -> "invalid"
WorkIntact == work = Request
=============================================================================

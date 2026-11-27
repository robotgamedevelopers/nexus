----------------------- MODULE EnrollmentCancel -----------------------
EXTENDS Naturals
\* Provider-independent cancellation of one owned enrollment. An accepted callback remains in verification.
Phases == {"needed", "started", "url-sent", "waiting", "callback-forwarded", "captured", "failed", "cancelled", "cancelling"}
Cancellable == {"started", "url-sent", "waiting"}
Eligible(p, matching, owner, nonempty) == p \in Cancellable /\ matching /\ owner /\ nonempty
Result(p, matching, owner, nonempty) == IF Eligible(p, matching, owner, nonempty) THEN "cancelling" ELSE p
Drained(p) == IF p = "cancelling" THEN "cancelled" ELSE p
VARIABLE before, after, matching, owner, nonempty, stopped
vars == <<before, after, matching, owner, nonempty, stopped>>
Init == /\ before \in Phases /\ after = before /\ matching = FALSE /\ owner = FALSE /\ nonempty = FALSE /\ stopped = FALSE
Next == \E p \in Phases, m, o, n \in BOOLEAN :
    /\ before' = p /\ matching' = m /\ owner' = o /\ nonempty' = n
    /\ after' = Result(p, m, o, n) /\ stopped' = Eligible(p, m, o, n)
Spec == Init /\ [][Next]_vars
OnlyOwnedPending == stopped => matching /\ owner /\ nonempty /\ before \in Cancellable
AcceptedCallbackKept == before = "callback-forwarded" => after = before /\ ~stopped
NoForeignEffect == (~matching \/ ~owner \/ ~nonempty) => after = before /\ ~stopped
=============================================================================

------------------------ MODULE LoginPreparation ------------------------
EXTENDS Naturals
\* The client only prepares requests. Acknowledgment alone is not success: the host
\* must also observe this request's durable signout notice and unsigned account state.
CONSTANT BrowserRequired
VARIABLES phase, last, from
vars == <<phase, last, from>>
Inputs == [valid : BOOLEAN, busy : BOOLEAN, acknowledged : BOOLEAN,
           signedOut : BOOLEAN, selected : BOOLEAN, confirmed : BOOLEAN]
Empty == [valid |-> TRUE, busy |-> FALSE, acknowledged |-> FALSE,
          signedOut |-> FALSE, selected |-> FALSE, confirmed |-> FALSE]
Init == phase = "waiting" /\ last = Empty /\ from = "waiting"
Result(p, i) ==
    IF ~i.valid THEN "failed"
    ELSE CASE p = "waiting" -> IF i.busy THEN p ELSE "signout"
           [] p = "signout" -> IF ~i.acknowledged THEN p ELSE IF i.signedOut THEN "select" ELSE "failed"
           [] p = "select" -> IF ~i.acknowledged THEN p ELSE
                 IF i.selected /\ i.signedOut THEN IF BrowserRequired THEN "browser" ELSE "ready" ELSE "failed"
           [] p = "browser" -> IF ~i.confirmed THEN p ELSE
                 IF i.selected /\ i.signedOut THEN "ready" ELSE "failed"
           [] OTHER -> p
Step(i) == phase' = Result(phase, i) /\ last' = i /\ from' = phase
Next == \E i \in Inputs : Step(i)
Spec == Init /\ [][Next]_vars
TypeOK == phase \in {"waiting", "signout", "select", "browser", "ready", "failed"}
NoSelectionBeforeSignout == phase = "select" /\ from # phase => last.acknowledged /\ last.signedOut
NoBrowserBeforeSelection == phase = "browser" /\ from # phase => last.acknowledged /\ last.selected /\ last.signedOut
NoEnrollmentBeforeConfirmation == phase = "ready" /\ from # phase /\ BrowserRequired =>
    from = "browser" /\ last.confirmed /\ last.selected /\ last.signedOut
FailureStops == from = "failed" => phase = "failed"
=============================================================================

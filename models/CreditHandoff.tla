----------------------------- MODULE CreditHandoff -----------------------------
\* One unfinished request with a partial reply. A limit is reported on the account
\* holding it. The server may move that same work to the next eligible subscription.
\* The fixture keeps the roster fixed for this work; each initial subset represents
\* a different mix of signed-in subscriptions. The responder's durable retry set is
\* the prefix of payers already tried.
EXTENDS Sequences, FiniteSets, Naturals

Order == <<"a", "b", "c">>
Accounts == {Order[i] : i \in DOMAIN Order}
Request == <<"person", "request">>
Prefix == <<Request, <<"friend", "partial">>>>

VARIABLES pinned, eligible, session, content, payers, stopped
vars == <<pinned, eligible, session, content, payers, stopped>>
Current == payers[Len(payers)]
Tried == {payers[i] : i \in DOMAIN payers}

Init ==
    /\ pinned \in BOOLEAN
    /\ eligible \in SUBSET Accounts \ {{}}
    /\ session = 1 /\ content = Prefix /\ stopped = FALSE
    /\ \E a \in eligible : payers = <<a>>

After(a) ==
    LET i == CHOOSE n \in DOMAIN Order : Order[n] = a
    IN SubSeq(Order, i + 1, Len(Order)) \o SubSeq(Order, 1, i - 1)

Advance(account, history) ==
    /\ payers' = Append(payers, account)
    /\ content' = history /\ stopped' = FALSE
    /\ UNCHANGED <<session, pinned, eligible>>

Halt ==
    /\ stopped' = TRUE
    /\ UNCHANGED <<pinned, eligible, session, content, payers>>

LimitWithout(exclusions, respectPin, history) ==
    /\ ~stopped
    /\ LET targets == SelectSeq(After(Current), LAMBDA a : a \in eligible \ exclusions)
       IN IF (respectPin /\ pinned) \/ targets = <<>> THEN Halt ELSE Advance(Head(targets), history)

Limit == LimitWithout(Tried, TRUE, content)
Spec == Init /\ [][Limit]_vars /\ WF_vars(Limit)

OnlyEligible == Tried \subseteq eligible
NeverReturn == Len(payers) = Cardinality(Tried)
PinHolds == pinned => Len(payers) = 1
WorkIntact == session = 1 /\ content = Prefix
NoPrematureStop == stopped => pinned \/ Tried = eligible
EventuallyStops == <>stopped

\* Deliberate faults: forget earlier failed payers, rotate a pin, or resubmit
\* the original user prompt into the transcript on the replacement account.
Bounce == LimitWithout({}, TRUE, content)
IgnorePin == LimitWithout(Tried, FALSE, content)
Duplicate == LimitWithout(Tried, TRUE, Append(content, Request))
BounceSpec == Init /\ [][Bounce]_vars
IgnorePinSpec == Init /\ [][IgnorePin]_vars
DuplicateSpec == Init /\ [][Duplicate]_vars
=============================================================================

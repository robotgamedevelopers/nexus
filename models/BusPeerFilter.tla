--------------------------- MODULE BusPeerFilter ---------------------------
\* One inbox, one candidate sender, two delivery IDs. A completed registry read
\* supplies evidence for a delivery only if it started after that delivery
\* was observed. BusDelivery models the flush of the resulting receipt.
EXTENDS Naturals

CONSTANT Fault
Ids == {1, 2}
\* A genuinely empty registry withdraws the inbox grant as well as every sender.
\* "other" is a valid registry retaining the inbox but omitting the candidate sender.
Answers == {"unavailable", "empty", "other", "friend"}
VARIABLES id, fresh, history, receipts, stored, decision, expected
vars == <<id, fresh, history, receipts, stored, decision, expected>>
Init == /\ id = 0 /\ fresh = "unavailable" /\ history = "unavailable"
        /\ receipts = {} /\ stored = {} /\ decision = "wait" /\ expected = "wait"

Offer(i) == /\ id' = i /\ fresh' = "unavailable"
            /\ UNCHANGED <<history, receipts, stored, decision, expected>>

Read(answer, beganAfterOffer) ==
    /\ fresh' = IF beganAfterOffer THEN answer ELSE "unavailable"
    /\ history' = IF answer = "unavailable" THEN history ELSE answer
    /\ UNCHANGED <<id, receipts, stored, decision, expected>>

Classify(answer) == IF answer \in {"unavailable", "empty"} THEN "wait" ELSE answer
Decide(fault) ==
    /\ id \in Ids
    /\ LET correct == IF id \in receipts THEN "replay" ELSE Classify(fresh)
           used == CASE fault = "use-cache" /\ correct = "wait" /\ history = "friend" -> "friend"
                     [] fault = "filter-outage" /\ correct = "wait" -> "other"
                     [] fault = "stall-unknown" /\ correct = "other" -> "wait"
                     [] fault = "forget-filter" /\ id \in receipts \ stored /\ fresh = "friend" -> "friend"
                     [] OTHER -> correct
       IN /\ expected' = correct /\ decision' = used
          /\ receipts' = IF used \in {"friend", "other"} THEN receipts \cup {id} ELSE receipts
          /\ stored' = IF used = "friend" THEN stored \cup {id} ELSE stored
    /\ UNCHANGED <<id, fresh, history>>

Next(fault) == \/ \E i \in Ids : Offer(i)
               \/ \E answer \in Answers, beganAfterOffer \in BOOLEAN : Read(answer, beganAfterOffer)
               \/ Decide(fault)
Spec == Init /\ [][Next(Fault)]_vars

DecisionMatchesEvidence == decision = expected
AllStoredHaveReceipts == stored \subseteq receipts
=============================================================================

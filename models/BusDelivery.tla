----------------------------- MODULE BusDelivery -----------------------------
\* Two inboxes receive the same stream ID. The receipt key includes the inbox.
\* Hub::receiveBus commits the receipt, bubble and prepared turn in one
\* journal record. admissions counts records even after their bubbles clear.
\* LoopPass models a crash anywhere in a batch. For one admission the record
\* either survives (Flush, then Restart) or does not (Restart before Flush).
EXTENDS Naturals, Sequences

Inboxes == {"ada", "bo"}
Empty == [receipts |-> {}, visible |-> [i \in Inboxes |-> 0],
          admissions |-> [i \in Inboxes |-> 0]]

VARIABLES mem, disk, pending, ready, acknowledged
vars == <<mem, disk, pending, ready, acknowledged>>
Init == /\ mem = Empty /\ disk = Empty /\ pending = {} /\ ready = {} /\ acknowledged = {}

Receive(i, seen) ==
    /\ i \notin pending \cup ready
    /\ mem' = IF seen THEN mem ELSE
          [mem EXCEPT !.receipts = @ \cup {i}, !.admissions[i] = @ + 1, !.visible[i] = @ + 1]
    /\ pending' = pending \cup {i}
    /\ UNCHANGED <<disk, ready, acknowledged>>

Clear(i) == /\ mem' = [mem EXCEPT !.visible[i] = 0]
            /\ UNCHANGED <<disk, pending, ready, acknowledged>>

Flush == /\ disk' = mem /\ ready' = ready \cup pending /\ pending' = {}
         /\ UNCHANGED <<mem, acknowledged>>

Reply(i, allowed) ==
    /\ i \in allowed
    /\ acknowledged' = acknowledged \cup {i}
    /\ ready' = ready \ {i} /\ pending' = pending \ {i}
    /\ UNCHANGED <<mem, disk>>

Restart == /\ mem' = disk /\ pending' = {} /\ ready' = {}
           /\ UNCHANGED <<disk, acknowledged>>

Advance(early, dedupFromBubble, globalReceipt) ==
    \/ \E i \in Inboxes :
          \/ Receive(i, IF dedupFromBubble THEN mem.visible[i] > 0
                        ELSE IF globalReceipt THEN mem.receipts # {} ELSE i \in mem.receipts)
          \/ Reply(i, IF early THEN pending \cup ready ELSE ready)
          \/ Clear(i)
    \/ Flush \/ Restart

Spec == Init /\ [][Advance(FALSE, FALSE, FALSE)]_vars
EarlyReply == Init /\ [][Advance(TRUE, FALSE, FALSE)]_vars
DedupFromBubble == Init /\ [][Advance(FALSE, TRUE, FALSE)]_vars
GlobalReceipt == Init /\ [][Advance(FALSE, FALSE, TRUE)]_vars

AckOnlyDurable == acknowledged \subseteq disk.receipts
AdmittedOnce == \A i \in Inboxes : mem.admissions[i] <= 1
ReceiptHasAdmission == \A i \in disk.receipts : disk.admissions[i] = 1
=============================================================================

---------------------------- MODULE WRlNoCarrier ----------------------------
\* Reversed witness: the carrier is never fair: the watch's frames may never get through. Delivered must reject it.
EXTENDS Relay

\* The fair spec without the carrier's fairness.

BrokenSpec == Spec /\ WF_vars(Hand) /\ WF_vars(DeliverCurrent) /\ WF_vars(Ack)
=============================================================================

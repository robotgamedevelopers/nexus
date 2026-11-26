---------------------------- MODULE WRelayResend ----------------------------
\* Reversed witness: a relay that ends has the actions written to it and not acknowledged written again to the
\* next relay, as a port that "loses nothing" would. The new relay is a new link with a new client id, so the server
\* cannot tell them from new ones: one whose acknowledgement was lost is applied twice. AppliedOnce must reject it.
EXTENDS RelayLife

Rewound == IF ackHigh > base THEN ackHigh ELSE base
BrokenDeath == DeathWith({}, Rewound, Doubled)
BrokenNext == (Next /\ ~Death) \/ BrokenDeath
BrokenSpec == Init /\ [][BrokenNext]_vars
=============================================================================

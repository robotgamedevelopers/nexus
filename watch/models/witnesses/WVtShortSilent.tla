---------------------------- MODULE WVtShortSilent ----------------------------
\* Reversed witness: a hold too short ends the turn and nothing is said. TurnEndsSaid must reject it.
EXTENDS VoiceTurn

Broken == phase = "listening" /\ phase' = Settle(Rest(connected, unapplied, awaiting), replies) /\ Did("release", TRUE)
          /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices, cues>>

BrokenSpec == Init /\ [][Next \/ (Broken /\ UNCHANGED unlocked)]_vars
=============================================================================

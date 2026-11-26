---------------------------- MODULE WVtSilentLost ----------------------------
\* Reversed witness: the server is lost and nothing is said. LostSays must reject it.
EXTENDS VoiceTurn

Broken == phase # "disconnected" /\ phase' = "disconnected" /\ connected' = FALSE /\ Did("lost", TRUE)
          /\ UNCHANGED <<current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices, cues>>

BrokenSpec == Init /\ [][Next \/ (Broken /\ UNCHANGED unlocked)]_vars
=============================================================================

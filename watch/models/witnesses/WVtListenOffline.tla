---------------------------- MODULE WVtListenOffline ----------------------------
\* Reversed witness: a press with no server starts the microphone. TalkOnlyConnected must reject it.
EXTENDS VoiceTurn

Broken == phase = "disconnected" /\ phase' = "listening" /\ Did("press", TRUE)
          /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices, cues>>

BrokenSpec == Init /\ [][Next \/ (Broken /\ UNCHANGED unlocked)]_vars
=============================================================================

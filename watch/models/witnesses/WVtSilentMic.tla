---------------------------- MODULE WVtSilentMic ----------------------------
\* Reversed witness: a press with the microphone off is refused and nothing is said. PressRefusedSays must reject it.
EXTENDS VoiceTurn

Broken == phase = "idle" /\ Did("press", FALSE) /\ UNCHANGED state /\ UNCHANGED <<notices, cues>>

BrokenSpec == Init /\ [][Next \/ (Broken /\ UNCHANGED unlocked)]_vars
=============================================================================

---------------------------- MODULE WVtListensLocked ----------------------------
\* Reversed witness: a press while the watch is locked starts listening. LockedQuiet must reject it.
EXTENDS VoiceTurn

Broken == /\ ~unlocked /\ connected /\ phase \in {"idle", "waiting"}
          /\ phase' = "listening" /\ Did("press", TRUE)
          /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, replies, notices, cues>>

BrokenSpec == Init /\ [][Next \/ (Broken /\ UNCHANGED unlocked)]_vars
=============================================================================

---------------------------- MODULE WVtSpeaksLocked ----------------------------
\* Reversed witness: a reply is read while the watch is locked (a reply arrives and is read whether or not the owner's
\* check has passed). LockedQuiet must reject it.
EXTENDS VoiceTurn

Broken == /\ ~unlocked /\ phase \in Resting /\ replies < MaxReplies
          /\ replies' = replies + 1 /\ phase' = "speaking" /\ Did("reply", TRUE)
          /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, notices, cues>>

BrokenSpec == Init /\ [][Next \/ (Broken /\ UNCHANGED unlocked)]_vars
=============================================================================

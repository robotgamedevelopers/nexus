---------------------------- MODULE WVtUnreadReply ----------------------------
\* Reversed witness: a reply read to its end rests the turn, though another reply waits. NoReplyLeftUnread must reject it.
EXTENDS VoiceTurn

Broken == phase = "speaking" /\ replies > 1 /\ replies' = replies - 1 /\ phase' = Rest(connected, unapplied, awaiting)
          /\ Did("spoken", TRUE) /\ UNCHANGED <<connected, current, utterance, heard, unapplied, nextPrompt, awaiting, notices, cues>>

BrokenSpec == Init /\ [][Next \/ (Broken /\ UNCHANGED unlocked)]_vars
=============================================================================

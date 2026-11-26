---------------------------- MODULE WVtUnconfirmed ----------------------------
\* Reversed witness: a prompt is made and who it went to is not said. PromptConfirmed must reject it.
EXTENDS VoiceTurn

Broken == phase = "choosing" /\ nextPrompt <= MaxPrompts
          /\ unapplied' = unapplied \cup {nextPrompt} /\ nextPrompt' = nextPrompt + 1
          /\ phase' = Settle(Rest(connected, unapplied', awaiting), replies) /\ Did("choose", TRUE)
          /\ UNCHANGED <<connected, current, utterance, heard, awaiting, replies, notices, cues>>

BrokenSpec == Init /\ [][Next \/ (Broken /\ UNCHANGED unlocked)]_vars
=============================================================================

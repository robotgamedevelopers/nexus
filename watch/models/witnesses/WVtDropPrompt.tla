---------------------------- MODULE WVtDropPrompt ----------------------------
\* Reversed witness: the server coming back forgets the prompts not yet applied. PromptsNeverDropped must reject it.
EXTENDS VoiceTurn

Broken == phase = "disconnected" /\ unapplied # {} /\ connected' = TRUE /\ unapplied' = {}
          /\ phase' = Settle(Rest(TRUE, {}, awaiting), replies) /\ Did("back", TRUE) /\ cues' = cues + 1
          /\ UNCHANGED <<current, utterance, heard, nextPrompt, awaiting, replies, notices>>

BrokenSpec == Init /\ [][Next \/ (Broken /\ UNCHANGED unlocked)]_vars
=============================================================================

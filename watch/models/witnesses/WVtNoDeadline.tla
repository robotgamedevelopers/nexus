---------------------------- MODULE WVtNoDeadline ----------------------------
\* Reversed witness: the clock never reaches the words' deadline (no fairness on Tick): the wait never ends. TranscribingEnds must reject it.
EXTENDS VoiceTurn

Broken == FALSE

BrokenSpec == Spec /\ WF_vars(SpeechDone /\ UNCHANGED <<cues, unlocked>>)
=============================================================================

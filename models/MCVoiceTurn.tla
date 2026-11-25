------------------------------ MODULE MCVoiceTurn ------------------------------
\* VoiceTurn.tla on two AIs, with at most two replies waiting to be spoken.
EXTENDS VoiceTurn

MCNone == "none"
QueueBound == Len(queue) <= 2
=============================================================================

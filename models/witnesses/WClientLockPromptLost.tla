---------------------- MODULE WClientLockPromptLost ----------------------
\* Reversed witness: a prompt that asks the system for nothing (its attempt never pending), so no answer ever comes and
\* the window waits on it for good. PromptEnds must reject it.
EXTENDS ClientLock

BrokenPrompt ==
    /\ state = "locked"
    /\ attempt < MaxAttempt
    /\ state' = "prompting"
    /\ attempt' = attempt + 1
    /\ UNCHANGED <<why, idle, granted, pending>>
    /\ last' = Rec("prompt", "", attempt + 1, 0, FALSE)

BrokenSpec == Init /\ [][NextWith(BrokenPrompt, Answer, Relock, Tick, Input, Frame)]_vars /\ WF_vars(AnswerSome)
=============================================================================

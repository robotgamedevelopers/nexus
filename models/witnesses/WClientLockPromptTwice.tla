---------------------- MODULE WClientLockPromptTwice ----------------------
\* Reversed witness: the hotkey pressed while the system prompt is up (or while unlocked) starts another prompt.
\* PromptOnlyLocked must reject it.
EXTENDS ClientLock

BrokenPrompt ==
    /\ attempt < MaxAttempt
    /\ state' = "prompting"
    /\ attempt' = attempt + 1
    /\ pending' = pending \cup {attempt + 1}
    /\ why' = IF state = "unlocked" THEN "launch" ELSE why
    /\ idle' = 0
    /\ granted' = 0
    /\ last' = Rec("prompt", "", attempt + 1, 0, FALSE)

BrokenSpec == Init /\ [][NextWith(BrokenPrompt, Answer, Relock, Tick, Input, Frame)]_vars
=============================================================================

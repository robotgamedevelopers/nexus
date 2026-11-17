------------------------------ MODULE WSignInAskKept ------------------------------
\* Reversed witness: a login stored through the capture (a late token, printed after the flow fell back to a paste)
\* leaves the paste asked for open, so the person is asked to paste a login that is already there. NoAskWhenSignedIn
\* must reject it.
EXTENDS MCSignIn

WriteKeepsAsk(a, v) ==
    IF disk = "ok"
    THEN /\ file' = [file EXCEPT ![a] = v] /\ signedin' = [signedin EXCEPT ![a] = TRUE] /\ UNCHANGED <<held, ask>>
    ELSE WriteCorrect(a, v)

BrokenSpec == Init /\ [][ServerNextWith(Need, Receive, LAMBDA a, v : PrintWith(WriteKeepsAsk, a, v), Timeout) \/
                         WindowNextWith(Redirect, Stray)]_vars
=============================================================================

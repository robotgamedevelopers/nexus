----------------------------- MODULE WSignInDropToken -----------------------------
\* Reversed witness: the token captured while the disk fails is written once and, when the write fails, dropped (not
\* held for another write), so the person must sign in again. TokenNeverDropped must reject it.
EXTENDS MCSignIn

WriteDrops(a, v) ==
    IF disk = "ok" THEN WriteCorrect(a, v) ELSE UNCHANGED <<file, signedin, held>>

BrokenSpec == Init /\ [][ServerNextWith(Need, Receive, LAMBDA a, v : PrintWith(WriteDrops, a, v), Timeout) \/
                         WindowNextWith(Redirect, Stray)]_vars
=============================================================================

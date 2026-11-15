----------------------------- MODULE WSignInNeedTwice -----------------------------
\* Reversed witness: a second report of the account's missing login (its driver again, the hourly check, an AI's
\* `config sign-in`) mints a second flow beside the live one. OneFlowPerAccount must reject it.
EXTENDS MCSignIn

BrokenSpec == Init /\ [][ServerNextWith(LAMBDA a : NeedWith(LAMBDA x : TRUE, a), Receive, Print, Timeout) \/
                         WindowNextWith(Redirect, Stray)]_vars
=============================================================================

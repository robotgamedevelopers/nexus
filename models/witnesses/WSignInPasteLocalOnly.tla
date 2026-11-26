------------------------- MODULE WSignInPasteLocalOnly -------------------------
\* The design before the paste: only a window on the server's machine may start an enrollment (the private page on the
\* server's loopback). With one window, on another machine, the account is never signed in: NeverSignedIn holds, where
\* MCSignInPasteRemote.cfg shows the paste path violates it (signs in).
EXTENDS SignInPaste

LocalOnlySpec == Init /\ [][ServerNextWith(TakesCorrect) \/ WindowNextWith(LAMBDA c : c \notin Remote)]_vars
=============================================================================

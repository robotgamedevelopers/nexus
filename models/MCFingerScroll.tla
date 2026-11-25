--------------------------- MODULE MCFingerScroll ---------------------------
\* FingerScroll.tla on a small list: a range of 0 to 3 rows (content of up to 5 rows in a view of 2), a drag shown under
\* 2 rows past an end (the view's height), a spring carried on out under 4.
EXTENDS FingerScroll

MCMaxes == 0 .. 3
=============================================================================

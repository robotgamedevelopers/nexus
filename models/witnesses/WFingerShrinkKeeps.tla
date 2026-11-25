---------------------------- MODULE WFingerShrinkKeeps ----------------------------
\* Reversed witness: a shorter range at rest leaves the offset where it was, past the new bottom. ShrinkPullsIn must
\* reject it.
EXTENDS MCFingerScroll

BrokenSpec == Init /\ [][NextWith(Down, Up, LAMBDA m : RangeWith(FALSE, TRUE, TRUE, m), Tick)]_vars
=============================================================================

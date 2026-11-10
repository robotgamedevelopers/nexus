------------------------------ MODULE WGlideJumpBack ------------------------------
\* Reversed witness: a key during a coast restarts it from where the coast began, so the page jumps back to its old
\* start. NeverBack must reject it.
EXTENDS MCGlide

JumpKey(d) ==
    IF Moving /\ mode = "coast" /\ Clamp(from) # pos
    THEN /\ Input /\ act' = "key"
         /\ pos' = Clamp(from) /\ over' = 0 /\ applied' = pos'
         /\ UNCHANGED <<mode, pending, target, from, dir, left, held, hdir, pressed, pageMoved>>
    ELSE Key(d)

BrokenSpec == Init /\ [][NextWith(Press, JumpKey, Wheel, Release, Frame, Interrupt)]_vars /\ Fairness
=============================================================================

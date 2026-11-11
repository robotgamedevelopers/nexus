--------------------------- MODULE WSupStopLatched ---------------------------
\* Reversed witness: a stop that finds nothing running is kept (latched), and stops the next turn to begin, one the
\* person never stopped. NoLatchedStop must reject it.
EXTENDS MCSupervisor

VARIABLE latch      \* a stop kept for later

\* What follows every step, then the kept stop written for a turn that just began.
Fire(x) == IF latch /\ x.phase = "working" THEN [x EXCEPT !.phase = "stopping", !.stops = Append(@, x.open)] ELSE x
A(x) == Fire(Advance(x))

KeepingStop ==
    /\ Stop
    /\ latch' = (latch \/ s.phase \notin {"working", "starting", "stopping"})

Others ==
    /\ \/ NextWithAll(PromptWith(A), QueuePromptWith(A), FALSE, LAMBDA m : MessageA(m, Gather, A),
                      ExitA(ExitedWith, A), TimeoutA(End, End, A), RestartA(Restarted, A))
       \/ BrokenA(A)
    /\ latch' = (latch /\ s'.phase # "stopping")

BrokenSpec == Init /\ latch = FALSE /\ [][KeepingStop \/ Others]_<<vars, latch>>
=============================================================================

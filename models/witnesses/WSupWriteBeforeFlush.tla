--------------------------- MODULE WSupWriteBeforeFlush ---------------------------
\* Reversed witness: a step's commands carried out before the step is durable. The server applies a batch of actions,
\* writes a prompt to the driver, and stops before the batch's one flush returns: the write went out, but the step's
\* durable fields (next, open, waiting, queue, session) are as they were before it. The server starts again from those,
\* with the prompt still waiting, and writes it again. AtMostOnce must reject it. The code holds every driver write
\* until the batch's flush has returned (Drivers::hold and Drivers::release, src/server/drivers.h).
EXTENDS MCSupervisor

WrittenNotDurable(x) ==
    LET y == Advance(x) IN
    [y EXCEPT !.next = x.next, !.open = x.open, !.waiting = x.waiting, !.queue = x.queue, !.session = x.session]

BrokenSpec == Init /\ [][NextWith(PromptWith(WrittenNotDurable), Message, Exit, Timeout, Restart)]_vars
=============================================================================

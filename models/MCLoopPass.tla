----------------------------- MODULE MCLoopPass -----------------------------
\* LoopPass.tla on small instances (MCLoopPass*.cfg), and the base of the reversed witnesses in
\* witnesses/WLoop*.tla.
EXTENDS LoopPass

\* Fairness of the server's own pass steps, the service restarting an exited (failed) server, and the
\* live driver's answers (never of crashes, failures or a person's actions): every window's actions
\* become durable, acknowledged and shown, and every turn settles.
RecoverFailed == \E k \in 0..Len(batch) : failed /\ Crash(k, FALSE)

ServerFairness ==
    /\ WF_vars(Tick(DefaultOrder))
    /\ WF_vars(Hold(DefaultOrder, TRUE))
    /\ SF_vars(DriverHello(DefaultOrder))
    /\ SF_vars(DriverDone(DefaultOrder))
    /\ \A c \in Clients : \A to \in 1..MaxActs : SF_vars(RecvOnly(DefaultOrder, c, to))
    /\ \A c \in Clients : \A to \in 1..MaxActs : SF_vars(ApplyAct(DefaultOrder, c, to))
    /\ WF_vars(Applied(DefaultOrder))
    /\ WF_vars(FlushOk(DefaultOrder))
    /\ WF_vars(Release(DefaultOrder))
    /\ WF_vars(Publish(DefaultOrder))
    /\ WF_vars(RecoverFailed)

LiveSpec == Spec /\ ServerFairness

Settled ==
    /\ ~failed
    /\ disk = mem
    /\ disk.open = 0 /\ disk.waiting = <<>> /\ disk.queue = <<>>
    /\ sentSnap = Snap(disk)
    /\ \A c \in Clients : acked[c] = Len(acts[c]) /\ disk.applied[c] = Len(acts[c])

Converges == <>[]Settled
=============================================================================

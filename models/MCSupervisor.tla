----------------------------- MODULE MCSupervisor -----------------------------
\* Supervisor.tla on small instances (MCSupervisor*.cfg), and the base of the reversed witnesses in
\* witnesses/WSup*.tla.
EXTENDS Supervisor

\* For an instance whose one change is not a sign-in (`SignIns <- NoSignIns` in its config).
NoSignIns == FALSE
=============================================================================

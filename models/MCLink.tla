------------------------------ MODULE MCLink ------------------------------
\* Link.tla on small instances (MCLink*.cfg), and the base of the reversed witnesses in witnesses/WLink*.tla. A
\* configuration file cannot write a sequence: these are the sessions the server holds at the start, for Start.
EXTENDS Link
StartNone == << >>
StartA == << "a" >>
StartAB == << "a", "b" >>
=============================================================================

------------------------------ MODULE WorkGroup ------------------------------
EXTENDS Naturals
VARIABLE opened
Init == opened = FALSE
Step(event) == IF event = "open" THEN opened' = TRUE
               ELSE IF event = "close" THEN opened' = FALSE
               ELSE UNCHANGED opened
=============================================================================

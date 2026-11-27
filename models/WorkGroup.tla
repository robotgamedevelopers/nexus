------------------------------ MODULE WorkGroup ------------------------------
EXTENDS Naturals
VARIABLE expanded
Init == expanded = FALSE
Step(event) == IF event = "toggle" THEN expanded' = ~expanded
               ELSE IF event = "reset" THEN expanded' = FALSE
               ELSE UNCHANGED expanded
=============================================================================

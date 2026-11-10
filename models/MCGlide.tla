------------------------------ MODULE MCGlide ------------------------------
\* Glide.tla on a small page: 4 scroll positions, a band of up to 2, a line of 1, a page of 2, an end of 3.
EXTENDS Glide

MCKeyDeltas == {-3, -2, 2, 3}
MCWheelDeltas == {-1, 1}
MCPadDeltas == {-1, 1}
MCSpots == {0, 3}
MCDts == {1, 2}

\* Glide's spec with the instance's bounds: the page past an end by less than Bound.
=============================================================================

// The VU interpreter's hot paths call small helpers (normalizeOperand, broadcast, flag
// updates) across these files; compiling them as one translation unit lets the compiler
// inline them, which matters because they run for every operand of every emulated cycle.
#include "ps2_vu1_core.cpp"
#include "ps2_vu1_upper.cpp"
#include "ps2_vu1_lower.cpp"

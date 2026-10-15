#pragma once

// VU1 interpreter speed options (kept out of ps2_vu1.h, which every generated file includes).

// Apply VF/VI/ACC results when an instruction issues instead of after their pipeline latency.
// Every reader stalls until the latest write to what it reads is ready, so programs compute the
// same results with the same cycle timing; only register state sampled mid-program (before the
// writeback cycle) differs. Flags, Q, P and stores keep their pipelines. Off by default.
void setVu1ImmediateWriteback(bool enabled);

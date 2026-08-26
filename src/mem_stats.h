#pragma once

// =====================================================================
// mem_stats.h — periodic process memory logging.
//
// Session 10: this project has a confirmed, still-unresolved memory leak
// (Task Manager private/working-set memory climbing without plateauing over
// long sessions, eventually crashing the process). Every diagnostic attempt
// so far has relied on the user manually watching Task Manager and
// describing what they saw after the fact. This puts the same numbers
// directly into coclassic.log on a timer, so growth can be correlated
// against whatever else was happening at that exact moment (which action
// fired, which scan completed, etc.) from the SAME log instead of two
// separate, hard-to-align timelines.
// =====================================================================

// Call every frame; internally throttled. Logs working set / private /
// pagefile usage (KB) at most once per interval.
void MaybeLogMemoryStats();

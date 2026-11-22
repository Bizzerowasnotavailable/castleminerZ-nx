#pragma once

// castleminerZ-nx performance monitor.
//
// Writes sdmc:/switch/castleminerz-logs/perf.log every few seconds: frame
// pacing, time spent in FNA3D by category, Mono GC pauses, and CPU time of the
// main thread and of every managed thread. When
// sdmc:/switch/castleminerz-logs/profile.ini exists it also samples call stacks
// into profile.bin. See docs/performance.md for the fields and the analysis
// script.

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*CmzPerfLogFn)(const char* line);

// Call once on the main thread before Mono starts. `enabled` comes from the
// SD card config (default on); when off, the perf threads, perf.log, notes and
// the FNA3D call accounting are skipped. Never fails: problems are written to
// the host log and the affected statistics are left out.
void cmz_perf_initialize(CmzPerfLogFn host_log, bool enabled);

// Installs the Mono profiler callbacks (GC pauses, managed thread start, name
// and stop). Call after mono_jit_init and before mono_jit_exec.
void cmz_perf_attach_runtime(void);

// Queues one line for perf.log, written before the next report and prefixed
// with "# ". Safe from any thread, including the audio thread; it never waits
// for I/O. Lines beyond the queue's capacity are dropped and counted.
void cmz_perf_note(const char* text);

#ifdef __cplusplus
}
#endif
